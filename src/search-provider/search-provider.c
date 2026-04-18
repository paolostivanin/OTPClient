#include <gio/gio.h>
#include <glib.h>
#include <jansson.h>
#include <libsecret/secret.h>
#include <gcrypt.h>
#include <cotp.h>
#include <time.h>

#include "../common/common.h"
#include "../common/db-common.h"
#include "../common/file-size.h"
#include "../common/secret-schema.h"
#include "../common/gsettings-common.h"

#define KRUNNER_BUS "com.github.paolostivanin.OTPClient.KRunner"
#define KRUNNER_PATH "/com/github/paolostivanin/OTPClient/KRunner"
#define GNOME_BUS "com.github.paolostivanin.OTPClient.SearchProvider"
#define GNOME_PATH "/com/github/paolostivanin/OTPClient/SearchProvider"

static gint32 global_max_file_size = 0;

#define CACHE_TTL_SECONDS 5

static GPtrArray *cached_entries = NULL;
static gint64     cached_at = 0;

typedef struct otp_search_entry_t {
    gchar *id;
    gchar *label;
    gchar *issuer;
    gchar *otp_value;
    gchar *db_name;
} OtpSearchEntry;

static void otp_search_entry_free (OtpSearchEntry *entry);
static GPtrArray *load_entries_uncached (void);
static GPtrArray *get_entries (void);
static gboolean entry_matches_terms (const OtpSearchEntry *entry, gchar **terms, gsize terms_len);
static gchar *get_entry_otp_value (json_t *obj);
static void send_notification (const gchar *label, const gchar *otp_value);

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




static void
otp_search_entry_free (OtpSearchEntry *entry)
{
    if (!entry) return;
    g_free (entry->id);
    g_free (entry->label);
    g_free (entry->issuer);
    g_free (entry->otp_value);
    g_free (entry->db_name);
    g_free (entry);
}


static gchar *
get_entry_otp_value (json_t *obj)
{
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


static void
load_entries_from_db (GPtrArray   *entries,
                      const gchar *db_path,
                      const gchar *db_name,
                      guint        db_index)
{
    if (!gsettings_common_get_use_secret_service ())
        return;

    gchar *pwd = secret_password_lookup_sync (OTPCLIENT_SCHEMA, NULL, NULL,
                                               "string", db_path, NULL);
    if (pwd == NULL)
        return;

    DatabaseData *db_data = g_new0 (DatabaseData, 1);
    db_data->db_path = g_strdup (db_path);
    db_data->key = secure_strdup (pwd);
    secret_password_free (pwd);
    db_data->max_file_size_from_memlock = global_max_file_size;

    GError *err = NULL;
    load_db (db_data, &err);
    if (err != NULL || db_data->in_memory_json_data == NULL)
    {
        if (err != NULL) g_clear_error (&err);
        gcry_free (db_data->key);
        g_slist_free_full (db_data->objects_hash, g_free);
        g_free (db_data->db_path);
        g_free (db_data);
        return;
    }

    gsize index;
    json_t *obj;
    json_array_foreach (db_data->in_memory_json_data, index, obj)
    {
        const gchar *label = json_string_value (json_object_get (obj, "label"));
        if (label == NULL) continue;
        OtpSearchEntry *entry = g_new0 (OtpSearchEntry, 1);
        entry->id = g_strdup_printf ("%u:%" G_GSIZE_FORMAT, db_index, index);
        entry->label = g_strdup (label);
        const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
        entry->issuer = g_strdup (issuer ? issuer : "");
        entry->otp_value = get_entry_otp_value (obj);
        entry->db_name = g_strdup (db_name);
        g_ptr_array_add (entries, entry);
    }

    gcry_free (db_data->key);
    json_decref (db_data->in_memory_json_data);
    g_slist_free_full (db_data->objects_hash, g_free);
    g_free (db_data->db_path);
    g_free (db_data);
}

static GPtrArray *
load_entries_uncached (void)
{
    GPtrArray *entries = g_ptr_array_new_with_free_func ((GDestroyNotify) otp_search_entry_free);

    g_autoptr (GPtrArray) db_list = gsettings_common_get_db_list ();
    if (db_list != NULL && db_list->len > 0)
    {
        for (guint i = 0; i < db_list->len; i++)
        {
            DbListEntry *dbe = g_ptr_array_index (db_list, i);
            load_entries_from_db (entries, dbe->path, dbe->name, i);
        }
    }
    else
    {
        /* Fallback: single-DB mode using db-path setting */
        g_autofree gchar *db_path = gsettings_common_get_db_path ();
        if (db_path != NULL)
            load_entries_from_db (entries, db_path, NULL, 0);
    }

    return entries;
}


static GPtrArray *
get_entries (void)
{
    gint64 now = time (NULL);
    if (cached_entries != NULL && (now - cached_at) < CACHE_TTL_SECONDS)
        return cached_entries;

    if (cached_entries != NULL)
        g_ptr_array_free (cached_entries, TRUE);

    cached_entries = load_entries_uncached ();
    cached_at = now;
    return cached_entries;
}


static gboolean
entry_matches_terms (const OtpSearchEntry *entry,
                     gchar               **terms,
                     gsize                 terms_len)
{
    if (terms_len == 0 || !entry->label) return FALSE;
    g_autofree gchar *l_fold = g_utf8_casefold (entry->label, -1);
    g_autofree gchar *i_fold = g_utf8_casefold (entry->issuer ? entry->issuer : "", -1);
    for (gsize i = 0; i < terms_len; i++) {
        if (!terms[i]) continue;
        g_autofree gchar *t_fold = g_utf8_casefold (terms[i], -1);
        if (!g_strstr_len (l_fold, -1, t_fold) && !g_strstr_len (i_fold, -1, t_fold))
            return FALSE;
    }
    return TRUE;
}


static void
send_notification (const gchar *label,
                   const gchar *otp_value)
{
    if (!otp_value) return;
    GDBusConnection *conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
    if (!conn) return;

    g_autofree gchar *body = g_strdup_printf ("Your code for %s is: %s",
                                               label ? label : "Account", otp_value);
    GVariantBuilder actions, hints;
    g_variant_builder_init (&actions, G_VARIANT_TYPE ("as"));
    g_variant_builder_init (&hints, G_VARIANT_TYPE ("a{sv}"));
    GVariant *reply = g_dbus_connection_call_sync (conn,
                                                    "org.freedesktop.Notifications",
                                                    "/org/freedesktop/Notifications",
                                                    "org.freedesktop.Notifications",
                                                    "Notify",
                                                    g_variant_new ("(susssasa{sv}i)",
                                                                   "OTPClient", (guint32)0,
                                                                   "com.github.paolostivanin.OTPClient",
                                                                   "OTP Token", body,
                                                                   &actions, &hints, (gint32)5000),
                                                    NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    if (reply != NULL)
        g_variant_unref (reply);
    g_object_unref (conn);
}


static void
handle_gnome_call (GDBusConnection       *conn,
                   const gchar           *sender,
                   const gchar           *path,
                   const gchar           *iface,
                   const gchar           *method,
                   GVariant              *params,
                   GDBusMethodInvocation *inv,
                   gpointer               data)
{
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
        GPtrArray *entries = get_entries ();
        for (guint i = 0; i < entries->len; i++) {
            OtpSearchEntry *e = g_ptr_array_index (entries, i);
            if (entry_matches_terms (e, terms, g_strv_length (terms)))
                g_variant_builder_add (&builder, "s", e->id);
        }
        g_dbus_method_invocation_return_value (inv, g_variant_new ("(as)", &builder));
        g_strfreev (terms);
    } else if (g_strcmp0 (method, "GetResultMetas") == 0) {
        gchar **ids;
        g_variant_get (params, "(^as)", &ids);
        GVariantBuilder builder;
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("aa{sv}"));
        GPtrArray *entries = get_entries ();
        for (gsize j = 0; ids[j]; j++) {
            for (guint i = 0; i < entries->len; i++) {
                OtpSearchEntry *e = g_ptr_array_index (entries, i);
                if (g_strcmp0 (e->id, ids[j]) == 0) {
                    GVariantBuilder meta;
                    g_variant_builder_init (&meta, G_VARIANT_TYPE ("a{sv}"));
                    g_variant_builder_add (&meta, "{sv}", "id", g_variant_new_string (e->id));
                    g_variant_builder_add (&meta, "{sv}", "name", g_variant_new_string (e->label));
                    g_autofree gchar *desc = NULL;
                    if (e->db_name != NULL && e->db_name[0] != '\0')
                        desc = g_strdup_printf ("%s — %s", e->db_name,
                                                (e->issuer && e->issuer[0]) ? e->issuer : e->label);
                    else
                        desc = g_strdup (e->issuer ? e->issuer : "");
                    g_variant_builder_add (&meta, "{sv}", "description", g_variant_new_string (desc));
                    g_variant_builder_add (&meta, "{sv}", "icon", g_variant_new_string ("com.github.paolostivanin.OTPClient"));
                    g_variant_builder_add_value (&builder, g_variant_builder_end (&meta));
                }
            }
        }
        g_dbus_method_invocation_return_value (inv, g_variant_new ("(aa{sv})", &builder));
        g_strfreev (ids);
    } else if (g_strcmp0 (method, "ActivateResult") == 0) {
        const gchar *id;
        g_variant_get (params, "(&s^as u)", &id, NULL, NULL);
        g_autofree gchar *otp = NULL;
        g_autofree gchar *label = NULL;
        GPtrArray *entries = get_entries ();
        for (guint i = 0; i < entries->len; i++) {
            OtpSearchEntry *e = g_ptr_array_index (entries, i);
            if (g_strcmp0 (e->id, id) == 0) {
                otp = g_strdup (e->otp_value);
                label = g_strdup (e->label);
                break;
            }
        }
        send_notification (label, otp);
        g_dbus_method_invocation_return_value (inv, NULL);
    } else {
        g_dbus_method_invocation_return_value (inv, NULL);
    }
}


static void
handle_krunner_call (GDBusConnection       *conn,
                     const gchar           *sender,
                     const gchar           *path,
                     const gchar           *iface,
                     const gchar           *method,
                     GVariant              *params,
                     GDBusMethodInvocation *inv,
                     gpointer               data)
{
    (void)conn; (void)sender; (void)path; (void)iface; (void)data;

    if (g_strcmp0 (method, "Match") == 0) {
        const gchar *query;
        g_variant_get (params, "(&s)", &query);
        GVariantBuilder builder;
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sssida{sv})"));
        if (query && *query) {
            g_auto(GStrv) terms = g_strsplit_set (query, " \t", -1);
            gsize terms_len = g_strv_length (terms);
            GPtrArray *entries = get_entries ();
            for (guint i = 0; i < entries->len; i++) {
                OtpSearchEntry *e = g_ptr_array_index (entries, i);
                if (!entry_matches_terms (e, terms, terms_len)) continue;
                if (!e->otp_value) continue;
                GVariantBuilder props;
                g_variant_builder_init (&props, G_VARIANT_TYPE ("a{sv}"));
                // Deliberately do NOT include the OTP value in the subtitle:
                // any process on the session bus can poll Match. The code is
                // only handed out via Run, where the user sees a notification.
                g_autofree gchar *sub = NULL;
                if (e->db_name != NULL && e->db_name[0] != '\0')
                    sub = (e->issuer && *e->issuer)
                        ? g_strdup_printf ("%s — %s", e->db_name, e->issuer)
                        : g_strdup (e->db_name);
                else
                    sub = g_strdup (e->issuer ? e->issuer : "");
                g_variant_builder_add (&props, "{sv}", "subtext", g_variant_new_string (sub));
                g_variant_builder_add (&props, "{sv}", "category", g_variant_new_string ("OTPClient"));
                g_variant_builder_add (&builder, "(sssida{sv})",
                                       e->id, e->label,
                                       "com.github.paolostivanin.OTPClient",
                                       (gint32)0, (gdouble)1.0, &props);
            }
        }
        GVariant *res = g_variant_builder_end (&builder);
        g_dbus_method_invocation_return_value (inv, g_variant_new_tuple (&res, 1));
    } else if (g_strcmp0 (method, "Run") == 0) {
        const gchar *id;
        g_variant_get (params, "(&s&s)", &id, NULL);
        g_autofree gchar *otp = NULL;
        g_autofree gchar *label = NULL;
        GPtrArray *entries = get_entries ();
        for (guint i = 0; i < entries->len; i++) {
            OtpSearchEntry *e = g_ptr_array_index (entries, i);
            if (g_strcmp0 (e->id, id) == 0) {
                otp = g_strdup (e->otp_value);
                label = g_strdup (e->label);
                break;
            }
        }
        send_notification (label, otp);
        g_dbus_method_invocation_return_value (inv, NULL);
    } else if (g_strcmp0 (method, "Actions") == 0) {
        GVariant *empty = g_variant_new_array (G_VARIANT_TYPE ("(sss)"), NULL, 0);
        g_dbus_method_invocation_return_value (inv, g_variant_new_tuple (&empty, 1));
    }
}


static const GDBusInterfaceVTable k_vtable = { handle_krunner_call, NULL, NULL, {0} };
static const GDBusInterfaceVTable g_vtable = { handle_gnome_call, NULL, NULL, {0} };


static void
on_krunner_bus_acquired (GDBusConnection *conn,
                         const gchar     *name G_GNUC_UNUSED,
                         gpointer         data G_GNUC_UNUSED)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml (krunner_introspection_xml, &error);
    if (node)
        g_dbus_connection_register_object (conn, KRUNNER_PATH, node->interfaces[0], &k_vtable, NULL, NULL, NULL);
}


static void
on_gnome_bus_acquired (GDBusConnection *conn,
                       const gchar     *name G_GNUC_UNUSED,
                       gpointer         data G_GNUC_UNUSED)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml (gnome_introspection_xml, &error);
    if (node)
        g_dbus_connection_register_object (conn, GNOME_PATH, node->interfaces[0], &g_vtable, NULL, NULL, NULL);
}


static GMainLoop *main_loop = NULL;


static void
on_name_lost (GDBusConnection *conn G_GNUC_UNUSED,
              const gchar     *name,
              gpointer         data G_GNUC_UNUSED)
{
    g_printerr ("Lost (or failed to acquire) D-Bus name '%s'. Is another instance running?\n", name);
    if (main_loop != NULL)
        g_main_loop_quit (main_loop);
}


int
main (int    argc,
      char **argv)
{
    gboolean force_kde = FALSE, force_gnome = FALSE;
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0 (argv[i], "--kde") == 0) force_kde = TRUE;
        else if (g_strcmp0 (argv[i], "--gnome") == 0) force_gnome = TRUE;
    }

    if (!gsettings_common_get_search_provider_enabled ())
        return 0;

    if (!force_kde && !force_gnome) {
        const gchar *desktop = g_getenv ("XDG_CURRENT_DESKTOP");
        if (desktop) {
            g_autofree gchar *dl = g_ascii_strdown (desktop, -1);
            if (g_strstr_len (dl, -1, "kde") || g_strstr_len (dl, -1, "plasma"))
                force_kde = TRUE;
            else if (g_strstr_len (dl, -1, "gnome"))
                force_gnome = TRUE;
        }
    }
    if (!force_kde && !force_gnome)
        return 0;

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
    if (force_kde)
        g_bus_own_name (G_BUS_TYPE_SESSION, KRUNNER_BUS, G_BUS_NAME_OWNER_FLAGS_NONE,
                        on_krunner_bus_acquired, NULL, on_name_lost, NULL, NULL);
    if (force_gnome)
        g_bus_own_name (G_BUS_TYPE_SESSION, GNOME_BUS, G_BUS_NAME_OWNER_FLAGS_NONE,
                        on_gnome_bus_acquired, NULL, on_name_lost, NULL, NULL);
    g_main_loop_run (main_loop);
    g_main_loop_unref (main_loop);
    return 0;
}
