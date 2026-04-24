#include <jansson.h>
#include <glib/gi18n.h>
#include "settings-import-export.h"
#include "gsettings-common.h"

typedef enum {
    SETTING_BOOL,
    SETTING_INT,
    SETTING_UINT,
    SETTING_STRING
} SettingType;

typedef struct {
    const gchar *key;
    SettingType  type;
} SettingDef;

static const SettingDef exportable_settings[] = {
    { "show-next-otp",          SETTING_BOOL },
    { "notification-enabled",   SETTING_BOOL },
    { "search-column",          SETTING_INT },
    { "dark-theme",             SETTING_BOOL },
    { "auto-lock",              SETTING_BOOL },
    { "auto-lock-timeout",      SETTING_UINT },
    { "secret-service",         SETTING_BOOL },
    { "search-provider-enabled",SETTING_BOOL },
    { "show-validity-seconds",  SETTING_BOOL },
    { "validity-color",         SETTING_STRING },
    { "validity-warning-color", SETTING_STRING },
    { "show-sidebar",           SETTING_BOOL },
    { "minimize-to-tray",       SETTING_BOOL },
    { "hide-otps",              SETTING_BOOL },
    { "otp-reveal-timeout",     SETTING_UINT },
    { NULL, 0 }
};


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
            case SETTING_INT:
                json_object_set_new (root, def->key,
                    json_integer (g_settings_get_int (settings, def->key)));
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

    gchar *result = json_dumps (root, JSON_INDENT (2) | JSON_SORT_KEYS);
    json_decref (root);

    if (result == NULL) {
        g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "Failed to serialize settings to JSON");
    }

    return result;
}


gboolean
import_settings_from_json (const gchar *json_str,
                           GError     **err)
{
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

        switch (def->type) {
            case SETTING_BOOL:
                if (json_is_boolean (val))
                    g_settings_set_boolean (settings, def->key, json_boolean_value (val));
                break;
            case SETTING_INT:
                if (json_is_integer (val))
                    g_settings_set_int (settings, def->key, (gint) json_integer_value (val));
                break;
            case SETTING_UINT:
                if (json_is_integer (val))
                    g_settings_set_uint (settings, def->key, (guint) json_integer_value (val));
                break;
            case SETTING_STRING:
                if (json_is_string (val))
                    g_settings_set_string (settings, def->key, json_string_value (val));
                break;
        }
    }

    json_decref (root);
    return TRUE;
}
