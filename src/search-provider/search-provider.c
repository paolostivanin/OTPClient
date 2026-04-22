#include <gio/gio.h>
#include <glib.h>
#include <jansson.h>
#include <libsecret/secret.h>
#include <gcrypt.h>
#include <cotp.h>
#include <string.h>
#include <time.h>

/* Project-specific headers */
#include "../common/common.h"
#include "../common/db-common.h"
#include "../common/file-size.h"
#include "../common/secret-schema.h"

#define KRUNNER_BUS "com.github.paolostivanin.OTPClient.KRunner"
#define KRUNNER_PATH "/com/github/paolostivanin/OTPClient/KRunner"
#define GNOME_BUS "com.github.paolostivanin.OTPClient.SearchProvider"
#define GNOME_PATH "/com/github/paolostivanin/OTPClient/SearchProvider"

#define OTPCLIENT_SEARCH_KEYWORD_MAX_LEN 32

static gint32 global_max_file_size = 0;

/* Trigger keyword loaded once at startup. The daemon ignores any query whose
 * first whitespace-separated token doesn't equal g_keyword (case-insensitive).
 * An empty keyword disables the filter and falls back to plain substring
 * matching. Changes to the config key only take effect after the daemon
 * is restarted. */
static gchar *g_keyword = NULL;
static gchar *g_keyword_fold = NULL;

typedef struct otp_search_entry_t {
    gchar *id;
    gchar *label;
    gchar *issuer;
    gchar *otp_value;
} OtpSearchEntry;

/* Forward Declarations */
static void otp_search_entry_free (OtpSearchEntry *entry);
static GPtrArray *load_entries (void);
static gboolean entry_matches_terms (const OtpSearchEntry *entry, gchar **terms, gsize terms_len);
static gchar *get_entry_otp_value (json_t *obj);
static void send_notification (const gchar *label, const gchar *otp_value);
static void copy_to_clipboard (const gchar *otp_value, gboolean is_kde);
static gchar *get_db_path (void);
static gboolean get_use_secret_service (void);
static gboolean get_search_provider_enabled (void);
static gchar *get_search_provider_keyword (void);
static void load_keyword_config (void);
static gboolean strip_keyword_or_skip (gchar **terms, gchar ***out_terms);

/* --- Introspection XML --- */
static const gchar *krunner_introspection_xml =
"<node>"
"  <interface name='org.kde.krunner1'>"
"    <method name='Match'><arg type='s' name='query' direction='in'/><arg type='a(sssida{sv})' name='matches' direction='out'/></method>"
"    <method name='Run'><arg type='s' name='id' direction='in'/><arg type='s' name='actionId' direction='in'/></method>"
"    <method name='Actions'><arg type='a(sss)' name='actions' direction='out'/></method>"
"  </interface>"
"</node>";

static const gchar *gnome_introspection_xml =
"<node>"
"  <interface name='org.gnome.Shell.SearchProvider2'>"
"    <method name='GetInitialResultSet'><arg type='as' name='terms' direction='in'/><arg type='as' name='results' direction='out'/></method>"
"    <method name='GetSubsearchResultSet'><arg type='as' name='prev' direction='in'/><arg type='as' name='terms' direction='in'/><arg type='as' name='results' direction='out'/></method>"
"    <method name='GetResultMetas'><arg type='as' name='results' direction='in'/><arg type='aa{sv}' name='metas' direction='out'/></method>"
"    <method name='ActivateResult'><arg type='s' name='id' direction='in'/><arg type='as' name='terms' direction='in'/><arg type='u' name='t' direction='in'/></method>"
"    <method name='LaunchSearch'><arg type='as' name='terms' direction='in'/><arg type='u' name='t' direction='in'/></method>"
"  </interface>"
"</node>";

/* --- Helpers Implementation --- */

/* Returns a freshly-loaded GKeyFile from the user config, or NULL
 * if the file is missing or unreadable. Caller must g_key_file_free(). */
static GKeyFile *open_user_config (void) {
#ifdef IS_FLATPAK
    g_autofree gchar *cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#else
    g_autofree gchar *cfg_file_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
#endif
    if (!g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) return NULL;
    GKeyFile *kf = g_key_file_new ();
    if (!g_key_file_load_from_file (kf, cfg_file_path, G_KEY_FILE_NONE, NULL)) {
        g_key_file_free (kf);
        return NULL;
    }
    return kf;
}

static gchar *get_db_path (void) {
    GKeyFile *kf = open_user_config ();
    if (kf == NULL) return NULL;
    gchar *db_path = g_key_file_get_string (kf, "config", "db_path", NULL);
    g_key_file_free (kf);
    return db_path;
}

static gboolean get_use_secret_service (void) {
    GKeyFile *kf = open_user_config ();
    if (kf == NULL) return TRUE;
    gboolean use_secret_service = g_key_file_get_boolean (kf, "config", "use_secret_service", NULL);
    g_key_file_free (kf);
    return use_secret_service;
}

static gboolean get_search_provider_enabled (void) {
    GKeyFile *kf = open_user_config ();
    if (kf == NULL) return TRUE;
    GError *err = NULL;
    gboolean enabled = g_key_file_get_boolean (kf, "config", "search_provider_enabled", &err);
    if (err) { enabled = TRUE; g_clear_error (&err); }
    g_key_file_free (kf);
    return enabled;
}

static gchar *get_search_provider_keyword (void) {
    GKeyFile *kf = open_user_config ();
    if (kf == NULL) return g_strdup ("otp");
    gchar *keyword = g_key_file_get_string (kf, "config", "search_provider_keyword", NULL);
    g_key_file_free (kf);
    if (keyword == NULL) keyword = g_strdup ("otp");
    return keyword;
}

static void load_keyword_config (void) {
    g_free (g_keyword);
    g_free (g_keyword_fold);
    g_keyword = get_search_provider_keyword ();
    if (g_keyword == NULL) g_keyword = g_strdup ("");
    g_strstrip (g_keyword);
    /* Defense in depth against arbitrary config edits: a runaway-length
     * keyword would still get casefolded and compared on every query.
     * Truncate by UTF-8 character count so we don't slice a multi-byte
     * sequence. */
    glong char_len = g_utf8_strlen (g_keyword, -1);
    if (char_len > OTPCLIENT_SEARCH_KEYWORD_MAX_LEN) {
        const gchar *cut = g_utf8_offset_to_pointer (g_keyword, OTPCLIENT_SEARCH_KEYWORD_MAX_LEN);
        *((gchar *) cut) = '\0';
    }
    g_keyword_fold = g_utf8_casefold (g_keyword, -1);
}

/* Returns TRUE and writes the post-keyword tail into *out_terms (caller frees
 * with g_strfreev) when the first non-empty term equals the configured
 * keyword AND there is at least one further non-empty term. Returns FALSE and
 * leaves *out_terms NULL otherwise. If the keyword is empty/disabled, behaves
 * transparently: returns TRUE with the original terms duplicated. */
static gboolean strip_keyword_or_skip (gchar **terms, gchar ***out_terms) {
    *out_terms = NULL;
    if (terms == NULL) return FALSE;

    if (g_keyword_fold == NULL || g_keyword_fold[0] == '\0') {
        *out_terms = g_strdupv (terms);
        return TRUE;
    }

    gsize i = 0;
    while (terms[i] != NULL && terms[i][0] == '\0') i++;
    if (terms[i] == NULL) return FALSE;

    g_autofree gchar *first_fold = g_utf8_casefold (terms[i], -1);
    if (g_strcmp0 (first_fold, g_keyword_fold) != 0) return FALSE;

    GPtrArray *tail = g_ptr_array_new ();
    for (gsize j = i + 1; terms[j] != NULL; j++) {
        if (terms[j][0] != '\0') g_ptr_array_add (tail, g_strdup (terms[j]));
    }
    if (tail->len == 0) {
        g_ptr_array_free (tail, TRUE);
        return FALSE;
    }
    g_ptr_array_add (tail, NULL);
    *out_terms = (gchar **) g_ptr_array_free (tail, FALSE);
    return TRUE;
}

static void otp_search_entry_free (OtpSearchEntry *entry) {
    if (!entry) return;
    g_free (entry->id); g_free (entry->label); g_free (entry->issuer); g_free (entry->otp_value);
    g_free (entry);
}

static gchar *get_entry_otp_value (json_t *obj) {
    const gchar *secret = json_string_value (json_object_get (obj, "secret"));
    const gchar *type = json_string_value (json_object_get (obj, "type"));
    if (!secret || !type) return NULL;
    cotp_error_t cotp_err;
    const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
    gint digits = (gint)json_integer_value (json_object_get (obj, "digits"));
    gint algo = get_algo_int_from_str (json_string_value (json_object_get (obj, "algo")));
    gchar *token = NULL;
    if (g_ascii_strcasecmp (type, "TOTP") == 0) {
        gint period = (gint)json_integer_value (json_object_get (obj, "period"));
        glong current_ts = time (NULL);
        if (issuer != NULL && g_ascii_strcasecmp (issuer, "steam") == 0) {
            token = get_steam_totp_at (secret, current_ts, period, &cotp_err);
        } else {
            token = get_totp_at (secret, current_ts, digits, period, algo, &cotp_err);
        }
    } else if (g_ascii_strcasecmp (type, "HOTP") == 0) {
        gint64 counter = json_integer_value (json_object_get (obj, "counter"));
        token = get_hotp (secret, counter, digits, algo, &cotp_err);
    }
    if (token == NULL) return NULL;
    gchar *result = g_strdup (token);
    g_free (token);
    return result;
}

static GPtrArray *load_entries (void) {
    GPtrArray *entries = g_ptr_array_new_with_free_func ((GDestroyNotify)otp_search_entry_free);
    gchar *db_path = get_db_path ();
    if (!db_path) return entries;
    DatabaseData *db_data = g_new0 (DatabaseData, 1);
    db_data->db_path = db_path;
    db_data->max_file_size_from_memlock = global_max_file_size;
    if (get_use_secret_service ()) {
        gchar *pwd = secret_password_lookup_sync (OTPCLIENT_SCHEMA, NULL, NULL, "string", "main_pwd", NULL);
        if (!pwd) { g_free (db_data->db_path); g_free (db_data); return entries; }
        db_data->key = secure_strdup (pwd);
        secret_password_free (pwd);
    } else { g_free (db_data->db_path); g_free (db_data); return entries; }
    GError *err = NULL;
    load_db (db_data, &err);
    if (err || !db_data->in_memory_json_data) {
        if (err) {
            g_clear_error (&err);
        }
        gcry_free (db_data->key);
        g_slist_free_full (db_data->objects_hash, g_free);
        g_free (db_data->db_path);
        g_free (db_data);
        return entries;
    }
    gsize index; json_t *obj;
    json_array_foreach (db_data->in_memory_json_data, index, obj) {
        const gchar *label = json_string_value (json_object_get (obj, "label"));
        if (!label) continue;
        OtpSearchEntry *entry = g_new0 (OtpSearchEntry, 1);
        entry->id = g_strdup_printf ("%" G_GSIZE_FORMAT, index);
        entry->label = g_strdup (label);
        const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
        entry->issuer = g_strdup (issuer ? issuer : "");
        entry->otp_value = get_entry_otp_value (obj);
        g_ptr_array_add (entries, entry);
    }
    gcry_free (db_data->key);
    json_decref (db_data->in_memory_json_data);
    g_slist_free_full (db_data->objects_hash, g_free);
    g_free (db_data->db_path);
    g_free (db_data);
    return entries;
}

static gboolean entry_matches_terms (const OtpSearchEntry *entry, gchar **terms, gsize terms_len) {
    if (terms_len == 0 || !entry->label) return FALSE;
    g_autofree gchar *l_fold = g_utf8_casefold (entry->label, -1);
    g_autofree gchar *i_fold = g_utf8_casefold (entry->issuer ? entry->issuer : "", -1);
    for (gsize i = 0; i < terms_len; i++) {
        if (!terms[i]) continue;
        g_autofree gchar *t_fold = g_utf8_casefold (terms[i], -1);
        if (!g_strstr_len (l_fold, -1, t_fold) && !g_strstr_len (i_fold, -1, t_fold)) return FALSE;
    }
    return TRUE;
}

static gboolean copy_via_klipper (const gchar *otp_value) {
    GDBusConnection *conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
    if (!conn) return FALSE;
    GError *err = NULL;
    GVariant *result = g_dbus_connection_call_sync (conn,
            "org.kde.klipper", "/klipper", "org.kde.klipper.klipper",
            "setClipboardContents", g_variant_new ("(s)", otp_value),
            NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, &err);
    g_object_unref (conn);
    if (err) { g_error_free (err); return FALSE; }
    if (result) g_variant_unref (result);
    return TRUE;
}

static gboolean copy_via_subprocess (const gchar *otp_value) {
    const gchar *session = g_getenv ("XDG_SESSION_TYPE");
    gboolean is_wayland = (session && g_ascii_strcasecmp (session, "wayland") == 0);
    const gchar *argv_wl[] = { "wl-copy", NULL };
    const gchar *argv_x11[] = { "xclip", "-selection", "clipboard", NULL };
    const gchar **argv = is_wayland ? argv_wl : argv_x11;
    GError *err = NULL;
    GSubprocess *proc = g_subprocess_newv (argv,
            G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE,
            &err);
    if (!proc) { if (err) g_error_free (err); return FALSE; }
    GBytes *input = g_bytes_new (otp_value, strlen (otp_value));
    gboolean ok = g_subprocess_communicate (proc, input, NULL, NULL, NULL, &err);
    g_bytes_unref (input);
    if (err) g_error_free (err);
    g_object_unref (proc);
    return ok;
}

static void copy_to_clipboard (const gchar *otp_value, gboolean is_kde) {
    if (!otp_value) return;
    if (is_kde && copy_via_klipper (otp_value)) return;
    copy_via_subprocess (otp_value);
}

static void send_notification (const gchar *label, const gchar *otp_value) {
    if (!otp_value) return;
    GDBusConnection *conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
    if (!conn) return;
    g_autofree gchar *body = g_strdup_printf ("Your code for %s is: %s", label ? label : "Account", otp_value);
    GVariantBuilder actions, hints;
    g_variant_builder_init (&actions, G_VARIANT_TYPE ("as"));
    g_variant_builder_init (&hints, G_VARIANT_TYPE ("a{sv}"));
    g_dbus_connection_call (conn, "org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify",
                            g_variant_new ("(susssasa{sv}i)", "OTPClient", (guint32)0, "com.github.paolostivanin.OTPClient", "OTP Token", body, &actions, &hints, (gint32)5000),
                            NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    g_object_unref (conn);
}

/* --- Handlers --- */

static void handle_gnome_call (GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface,
                               const gchar *method, GVariant *params, GDBusMethodInvocation *inv, gpointer data) {
    (void)conn; (void)sender; (void)path; (void)iface; (void)data;
    if (g_strcmp0 (method, "GetInitialResultSet") == 0 || g_strcmp0 (method, "GetSubsearchResultSet") == 0) {
        gchar **terms;
        if (g_strcmp0 (method, "GetInitialResultSet") == 0) {
            g_variant_get (params, "(^as)", &terms);
        } else {
            gchar **prev_results = NULL;
            g_variant_get (params, "(^as^as)", &prev_results, &terms);
            g_strfreev (prev_results);
        }
        GVariantBuilder builder;
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("as"));
        g_auto(GStrv) stripped = NULL;
        if (strip_keyword_or_skip (terms, &stripped)) {
            GPtrArray *entries = load_entries ();
            gsize stripped_len = g_strv_length (stripped);
            for (guint i = 0; i < entries->len; i++) {
                OtpSearchEntry *e = g_ptr_array_index (entries, i);
                if (entry_matches_terms (e, stripped, stripped_len)) g_variant_builder_add (&builder, "s", e->id);
            }
            g_ptr_array_free (entries, TRUE);
        }
        g_dbus_method_invocation_return_value (inv, g_variant_new ("(as)", &builder));
        g_strfreev (terms);
    } else if (g_strcmp0 (method, "GetResultMetas") == 0) {
        gchar **ids;
        g_variant_get (params, "(^as)", &ids);
        GVariantBuilder builder;
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("aa{sv}"));
        GPtrArray *entries = load_entries ();
        for (gsize j = 0; ids[j]; j++) {
            for (guint i = 0; i < entries->len; i++) {
                OtpSearchEntry *e = g_ptr_array_index (entries, i);
                if (g_strcmp0 (e->id, ids[j]) == 0) {
                    GVariantBuilder meta;
                    g_variant_builder_init (&meta, G_VARIANT_TYPE ("a{sv}"));
                    g_variant_builder_add (&meta, "{sv}", "id", g_variant_new_string (e->id));
                    g_variant_builder_add (&meta, "{sv}", "name", g_variant_new_string (e->label));
                    g_variant_builder_add (&meta, "{sv}", "description", g_variant_new_string (e->issuer));
                    g_variant_builder_add (&meta, "{sv}", "icon", g_variant_new_string ("com.github.paolostivanin.OTPClient"));
                    g_variant_builder_add_value (&builder, g_variant_builder_end (&meta));
                }
            }
        }
        g_ptr_array_free (entries, TRUE);
        g_dbus_method_invocation_return_value (inv, g_variant_new ("(aa{sv})", &builder));
        g_strfreev (ids);
    } else if (g_strcmp0 (method, "ActivateResult") == 0) {
        const gchar *id;
        g_variant_get (params, "(&s^as u)", &id, NULL, NULL);
        g_autofree gchar *otp = NULL;
        g_autofree gchar *label = NULL;
        GPtrArray *entries = load_entries ();
        for (guint i = 0; i < entries->len; i++) {
            OtpSearchEntry *e = g_ptr_array_index (entries, i);
            if (g_strcmp0 (e->id, id) == 0) {
                otp = g_strdup (e->otp_value);
                label = g_strdup (e->label);
                break;
            }
        }
        g_ptr_array_free (entries, TRUE);
        copy_to_clipboard (otp, FALSE);
        send_notification (label, otp);
        g_dbus_method_invocation_return_value (inv, NULL);
    } else { g_dbus_method_invocation_return_value (inv, NULL); }
}

static void handle_krunner_call (GDBusConnection *conn, const gchar *sender, const gchar *path, const gchar *iface,
                                const gchar *method, GVariant *params, GDBusMethodInvocation *inv, gpointer data) {
    (void)conn; (void)sender; (void)path; (void)iface; (void)data;
    if (g_strcmp0 (method, "Match") == 0) {
        const gchar *query;
        g_variant_get (params, "(&s)", &query);
        GVariantBuilder builder;
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sssida{sv})"));
        if (query && *query) {
            g_auto(GStrv) terms = g_strsplit_set (query, " \t", -1);
            g_auto(GStrv) stripped = NULL;
            if (strip_keyword_or_skip (terms, &stripped)) {
                gsize stripped_len = g_strv_length (stripped);
                GPtrArray *entries = load_entries ();
                for (guint i = 0; i < entries->len; i++) {
                    OtpSearchEntry *e = g_ptr_array_index (entries, i);
                    if (!entry_matches_terms (e, stripped, stripped_len)) continue;
                    if (!e->otp_value) continue;
                    GVariantBuilder props;
                    g_variant_builder_init (&props, G_VARIANT_TYPE ("a{sv}"));
                    // Deliberately do NOT include the OTP in the subtitle:
                    // any process on the session bus can poll Match. The code
                    // is only delivered via Run, where the user sees the
                    // notification.
                    g_autofree gchar *sub = g_strdup (e->issuer ? e->issuer : "");
                    g_variant_builder_add (&props, "{sv}", "subtext", g_variant_new_string (sub));
                    g_variant_builder_add (&props, "{sv}", "category", g_variant_new_string ("OTPClient"));
                    g_variant_builder_add (&builder, "(sssida{sv})", e->id, e->label, "com.github.paolostivanin.OTPClient", (gint32)0, (gdouble)1.0, &props);
                }
                g_ptr_array_free (entries, TRUE);
            }
        }
        GVariant *res = g_variant_builder_end (&builder);
        g_dbus_method_invocation_return_value (inv, g_variant_new_tuple (&res, 1));
    } else if (g_strcmp0 (method, "Run") == 0) {
        const gchar *id;
        g_variant_get (params, "(&s&s)", &id, NULL);
        g_autofree gchar *otp = NULL;
        g_autofree gchar *label = NULL;
        GPtrArray *entries = load_entries ();
        for (guint i = 0; i < entries->len; i++) {
            OtpSearchEntry *e = g_ptr_array_index (entries, i);
            if (g_strcmp0 (e->id, id) == 0) {
                otp = g_strdup (e->otp_value);
                label = g_strdup (e->label);
                break;
            }
        }
        g_ptr_array_free (entries, TRUE);
        copy_to_clipboard (otp, TRUE);
        send_notification (label, otp);
        g_dbus_method_invocation_return_value (inv, NULL);
    } else if (g_strcmp0 (method, "Actions") == 0) {
        GVariant *empty = g_variant_new_array (G_VARIANT_TYPE ("(sss)"), NULL, 0);
        g_dbus_method_invocation_return_value (inv, g_variant_new_tuple (&empty, 1));
    }
}

static const GDBusInterfaceVTable k_vtable = { handle_krunner_call, NULL, NULL, {0} };
static const GDBusInterfaceVTable g_vtable = { handle_gnome_call, NULL, NULL, {0} };

static void on_krunner_bus_acquired (GDBusConnection *conn, const gchar *name G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml (krunner_introspection_xml, &error);
    if (node) g_dbus_connection_register_object (conn, KRUNNER_PATH, node->interfaces[0], &k_vtable, NULL, NULL, NULL);
}

static void on_gnome_bus_acquired (GDBusConnection *conn, const gchar *name G_GNUC_UNUSED, gpointer data G_GNUC_UNUSED) {
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml (gnome_introspection_xml, &error);
    if (node) g_dbus_connection_register_object (conn, GNOME_PATH, node->interfaces[0], &g_vtable, NULL, NULL, NULL);
}

static GMainLoop *main_loop = NULL;

static void on_name_lost (GDBusConnection *conn G_GNUC_UNUSED, const gchar *name, gpointer data G_GNUC_UNUSED) {
    g_printerr ("Lost (or failed to acquire) D-Bus name '%s'. Is another instance running?\n", name);
    if (main_loop != NULL) g_main_loop_quit (main_loop);
}

int main (int argc, char **argv) {
    gboolean force_kde = FALSE, force_gnome = FALSE;
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0 (argv[i], "--kde") == 0) force_kde = TRUE;
        else if (g_strcmp0 (argv[i], "--gnome") == 0) force_gnome = TRUE;
    }
    if (!get_search_provider_enabled ()) return 0;

    load_keyword_config ();

    if (!force_kde && !force_gnome) {
        const gchar *desktop = g_getenv ("XDG_CURRENT_DESKTOP");
        if (desktop) {
            g_autofree gchar *dl = g_ascii_strdown (desktop, -1);
            if (g_strstr_len (dl, -1, "kde") || g_strstr_len (dl, -1, "plasma")) force_kde = TRUE;
            else if (g_strstr_len (dl, -1, "gnome")) force_gnome = TRUE;
        }
    }
    if (!force_kde && !force_gnome) return 0;

    if (set_memlock_value (&global_max_file_size) == MEMLOCK_ERR) {
        g_printerr ("Couldn't get the memlock value.\n");
        return 1;
    }
    gchar *init_msg = init_libs (global_max_file_size);
    if (init_msg != NULL) {
        g_printerr ("Error while initializing GCrypt: %s\n", init_msg);
        g_free (init_msg);
        return 1;
    }

    main_loop = g_main_loop_new (NULL, FALSE);
    if (force_kde) g_bus_own_name (G_BUS_TYPE_SESSION, KRUNNER_BUS, G_BUS_NAME_OWNER_FLAGS_NONE, on_krunner_bus_acquired, NULL, on_name_lost, NULL, NULL);
    if (force_gnome) g_bus_own_name (G_BUS_TYPE_SESSION, GNOME_BUS, G_BUS_NAME_OWNER_FLAGS_NONE, on_gnome_bus_acquired, NULL, on_name_lost, NULL, NULL);
    g_main_loop_run (main_loop);
    g_main_loop_unref (main_loop);
    g_clear_pointer (&g_keyword, g_free);
    g_clear_pointer (&g_keyword_fold, g_free);
    return 0;
}
