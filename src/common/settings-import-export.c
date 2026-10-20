#define _GNU_SOURCE
#include <string.h>
#include <jansson.h>
#include <glib/gi18n.h>
#include "settings-import-export.h"
#include "gsettings-common.h"

typedef enum {
    SETTING_BOOL,
    SETTING_UINT,
    SETTING_STRING
} SettingType;

typedef struct {
    const gchar *key;
    SettingType  type;
    /* The key records a wish, not a fact: what it asks for is a login-time
     * entry or a background grant, both of which belong to the desktop and
     * neither of which a GSettings write touches. Importing one of these
     * leaves the key and the world disagreeing until somebody reconciles
     * them, so the import says when it has written one. */
    gboolean     startup;
} SettingDef;

static const SettingDef exportable_settings[] = {
    { "show-next-otp",          SETTING_BOOL,   FALSE },
    { "notification-enabled",   SETTING_BOOL,   FALSE },
    { "dark-theme",             SETTING_BOOL,   FALSE },
    { "auto-lock",              SETTING_BOOL,   FALSE },
    { "auto-lock-timeout",      SETTING_UINT,   FALSE },
    { "secret-service",         SETTING_BOOL,   FALSE },
    { "search-provider-enabled",SETTING_BOOL,   FALSE },
    { "search-provider-keyword",SETTING_STRING, FALSE },
    { "show-validity-seconds",  SETTING_BOOL,   FALSE },
    { "validity-color",         SETTING_STRING, FALSE },
    { "validity-warning-color", SETTING_STRING, FALSE },
    { "show-sidebar",           SETTING_BOOL,   FALSE },
    { "clipboard-clear-timeout",SETTING_UINT,   FALSE },
    { "minimize-to-tray",       SETTING_BOOL,   TRUE  },
    { "start-minimized",        SETTING_BOOL,   TRUE  },
    { "autostart",              SETTING_BOOL,   TRUE  },
    { "hide-otps",              SETTING_BOOL,   FALSE },
    { NULL, 0, FALSE }
};

static gboolean
is_valid_hex_color (const gchar *color)
{
    if (color == NULL || color[0] != '#')
        return FALSE;

    gsize len = strlen (color + 1);
    if (len != 3 && len != 4 && len != 6 && len != 8)
        return FALSE;
    for (const gchar *p = color + 1; *p != '\0'; p++)
        if (!g_ascii_isxdigit (*p))
            return FALSE;
    return TRUE;
}


gchar *
export_settings_to_json (GError **err)
{
    g_autoptr (GSettings) settings = gsettings_common_get_settings ();
    if (settings == NULL) {
        g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "GSettings schema not available");
        return NULL;
    }

    json_t *root = json_object ();

    for (gsize i = 0; exportable_settings[i].key != NULL; i++) {
        const SettingDef *def = &exportable_settings[i];
        switch (def->type) {
            case SETTING_BOOL:
                json_object_set_new (root, def->key,
                    json_boolean (g_settings_get_boolean (settings, def->key)));
                break;
            case SETTING_UINT:
                json_object_set_new (root, def->key,
                    json_integer (g_settings_get_uint (settings, def->key)));
                break;
            case SETTING_STRING: {
                g_autofree gchar *val = g_settings_get_string (settings, def->key);
                json_object_set_new (root, def->key, json_string (val));
                break;
            }
        }
    }

    /* Serialise into a buffer we own rather than taking json_dumps' allocation.
     * jansson's allocator is global and init_libs swaps it for libgcrypt's
     * secure one, so a json_dumps result has to be freed with gcry_free in the
     * GUI and with free in the CLI, which calls this before init_libs runs.
     * Callers cannot tell the two apart, and one of them was already getting it
     * wrong. A plain GLib buffer is the same answer under either allocator. */
    const size_t flags = JSON_INDENT (2) | JSON_SORT_KEYS;
    size_t needed = json_dumpb (root, NULL, 0, flags);
    gchar *result = NULL;
    if (needed > 0) {
        result = g_malloc (needed + 1);
        /* json_dumpb does not terminate the buffer, and we want a C string.
         * The second dump can fail like the first (returning 0); accepting it
         * used to hand back "" as a successful export. */
        size_t written = json_dumpb (root, result, needed, flags);
        if (written != needed) {
            g_free (result);
            result = NULL;
        } else {
            result[written] = '\0';
        }
    }
    json_decref (root);

    if (result == NULL) {
        g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "Failed to serialize settings to JSON");
    }

    return result;
}


gchar *
settings_json_read_file (const gchar *path,
                         GError     **err)
{
    g_autoptr (GFile) file = g_file_new_for_path (path);
    g_autoptr (GFileInputStream) input = g_file_read (file, NULL, err);
    if (input == NULL)
        return NULL;

    GByteArray *bytes = g_byte_array_sized_new (4096);
    guint8 buffer[4096];
    while (bytes->len <= SETTINGS_JSON_MAX_BYTES) {
        gsize capacity = MIN (sizeof buffer, SETTINGS_JSON_MAX_BYTES + 1 - bytes->len);
        gssize count = g_input_stream_read (G_INPUT_STREAM (input), buffer, capacity,
                                            NULL, err);
        if (count < 0) {
            g_byte_array_unref (bytes);
            return NULL;
        }
        if (count == 0)
            break;
        g_byte_array_append (bytes, buffer, (guint) count);
    }

    if (bytes->len > SETTINGS_JSON_MAX_BYTES) {
        g_byte_array_unref (bytes);
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "Settings JSON exceeds %u bytes; refusing to import.",
                     SETTINGS_JSON_MAX_BYTES);
        return NULL;
    }

    g_byte_array_append (bytes, (const guint8 *) "\0", 1);
    return (gchar *) g_byte_array_free (bytes, FALSE);
}

gboolean
import_settings_from_json (const gchar *json_str,
                           gboolean    *out_touched_startup,
                           GError     **err)
{
    gboolean touched_startup = FALSE;
    if (out_touched_startup != NULL)
        *out_touched_startup = FALSE;

    /* M6: every setting in `exportable_settings` is a small primitive (bool,
     * int, string), so a real backup is well under 1 KiB. Cap input at 1 MiB
     * so a malicious or accidental multi-gigabyte file can't drag jansson
     * through unbounded allocations before erroring out. */
    if (json_str == NULL) {
        g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "Settings JSON is NULL.");
        return FALSE;
    }
    if (strnlen (json_str, SETTINGS_JSON_MAX_BYTES + 1) > SETTINGS_JSON_MAX_BYTES) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "Settings JSON exceeds %u bytes; refusing to import.",
                     SETTINGS_JSON_MAX_BYTES);
        return FALSE;
    }
    json_error_t jerr;
    json_t *root = json_loads (json_str, 0, &jerr);
    if (root == NULL) {
        g_set_error (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                     "Failed to parse JSON: %s (line %d)", jerr.text, jerr.line);
        return FALSE;
    }

    if (!json_is_object (root)) {
        json_decref (root);
        g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             "JSON root must be an object");
        return FALSE;
    }

    g_autoptr (GSettings) settings = gsettings_common_get_settings ();
    if (settings == NULL) {
        json_decref (root);
        g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "GSettings schema not available");
        return FALSE;
    }

    for (gsize i = 0; exportable_settings[i].key != NULL; i++) {
        const SettingDef *def = &exportable_settings[i];
        json_t *val = json_object_get (root, def->key);
        if (val == NULL)
            continue;

        /* Tracked rather than assumed from the key being present: a value of
         * the wrong type is skipped, and reconciling over a key nobody managed
         * to write would mean a portal round trip for nothing. */
        gboolean applied = FALSE;

        switch (def->type) {
            case SETTING_BOOL:
                if (json_is_boolean (val)) {
                    g_settings_set_boolean (settings, def->key, json_boolean_value (val));
                    applied = TRUE;
                }
                break;
            case SETTING_UINT:
                if (json_is_integer (val)) {
                    json_int_t v = json_integer_value (val);
                    gboolean valid = v >= 0 && (guint64) v <= G_MAXUINT;
                    if (g_str_equal (def->key, "auto-lock-timeout"))
                        valid = v >= 1 && v <= G_MAXINT;
                    if (valid) {
                        g_settings_set_uint (settings, def->key, (guint) v);
                        applied = TRUE;
                    } else {
                        g_warning ("Skipping out-of-range unsigned setting '%s'.", def->key);
                    }
                }
                break;
            case SETTING_STRING:
                if (json_is_string (val)) {
                    const gchar *value = json_string_value (val);
                    gboolean valid = TRUE;
                    if (g_str_equal (def->key, "validity-color") ||
                        g_str_equal (def->key, "validity-warning-color"))
                        valid = is_valid_hex_color (value);
                    if (valid) {
                        g_settings_set_string (settings, def->key, value);
                        applied = TRUE;
                    } else {
                        g_warning ("Skipping invalid color setting '%s'.", def->key);
                    }
                }
                break;
        }

        if (applied && def->startup)
            touched_startup = TRUE;
    }

    /* Persisted, not just returned. The CLI cannot act on these keys at all and
     * the GUI can be killed between the write and the portal answering, so the
     * one durable record that the desktop has not been told yet has to outlive
     * the process that noticed. Cleared by the reconciliation, wherever it
     * eventually happens. */
    if (touched_startup)
        g_settings_set_boolean (settings, "startup-reconcile-pending", TRUE);

    if (out_touched_startup != NULL)
        *out_touched_startup = touched_startup;

    json_decref (root);
    return TRUE;
}
