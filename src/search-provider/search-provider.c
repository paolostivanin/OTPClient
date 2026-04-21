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

/* Cache TTL is a fallback: file monitors invalidate the cache eagerly when a
 * DB file is touched on disk. The TTL covers the case where the on-disk DB
 * is unchanged but a HOTP counter / secret service value moved underneath us. */
#define CACHE_TTL_SECONDS 60

static GPtrArray  *cached_entries = NULL;
static gint64      cached_at = 0;
/* path (gchar*) → GFileMonitor*. Diffed across reloads so monitors for
 * unchanged DB paths survive without a brief monitor-less window during
 * which writes would slip past the cache invalidator. */
static GHashTable *file_monitors = NULL;

typedef struct otp_search_entry_t {
    gchar *id;
    gchar *label;
    gchar *issuer;
    gchar *db_name;
    gchar *db_path;        /* needed to recompute OTP on Run/Activate */
    gsize  json_index;     /* position of the token within the DB's JSON array */
    /* Pre-folded copies for entry_matches_terms — avoid casefolding per query. */
    gchar *label_fold;
    gchar *issuer_fold;
} OtpSearchEntry;

static void otp_search_entry_free (OtpSearchEntry *entry);
static GPtrArray *load_entries_uncached (void);
static GPtrArray *get_entries (void);
static gboolean entry_matches_terms (const OtpSearchEntry *entry, gchar **terms, gsize terms_len);
static gchar *get_entry_otp_value (json_t *obj);
static gchar *compute_otp_for_entry (const OtpSearchEntry *entry);
static void send_notification (const gchar *label, const gchar *otp_value);
static void clear_file_monitors (void);
static void sync_file_monitors (GPtrArray *desired_paths);
static void on_db_file_changed (GFileMonitor *monitor, GFile *file, GFile *other,
                                GFileMonitorEvent event, gpointer user_data);
static gboolean prewarm_cache (gpointer user_data);

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
    g_free (entry->db_name);
    g_free (entry->db_path);
    g_free (entry->label_fold);
    g_free (entry->issuer_fold);
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
        db_invalidate_kdf_cache (db_data);
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
        const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
        OtpSearchEntry *entry = g_new0 (OtpSearchEntry, 1);
        entry->id = g_strdup_printf ("%u:%" G_GSIZE_FORMAT, db_index, index);
        entry->label = g_strdup (label);
        entry->issuer = g_strdup (issuer ? issuer : "");
        entry->db_name = g_strdup (db_name);
        entry->db_path = g_strdup (db_path);
        entry->json_index = index;
        /* Pre-casefold for entry_matches_terms — done once at load instead of
         * once per term per query. Live OTP codes are no longer cached: they're
         * recomputed on demand in compute_otp_for_entry, so a heap inspection
         * of the daemon shows only labels/issuers, not active codes. */
        entry->label_fold = g_utf8_casefold (entry->label, -1);
        entry->issuer_fold = g_utf8_casefold (entry->issuer, -1);
        g_ptr_array_add (entries, entry);
    }

    db_invalidate_kdf_cache (db_data);
    gcry_free (db_data->key);
    json_decref (db_data->in_memory_json_data);
    g_slist_free_full (db_data->objects_hash, g_free);
    g_free (db_data->db_path);
    g_free (db_data);
}

static void
on_db_file_changed (GFileMonitor      *monitor G_GNUC_UNUSED,
                    GFile             *file G_GNUC_UNUSED,
                    GFile             *other G_GNUC_UNUSED,
                    GFileMonitorEvent  event,
                    gpointer           user_data G_GNUC_UNUSED)
{
    /* Invalidate on any event that could change the DB contents. CHANGED
     * fires often during a write; CHANGES_DONE_HINT marks the end of a
     * write batch. Either way, drop the cache so the next query rebuilds. */
    if (event == G_FILE_MONITOR_EVENT_CHANGED ||
        event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
        event == G_FILE_MONITOR_EVENT_CREATED ||
        event == G_FILE_MONITOR_EVENT_DELETED ||
        event == G_FILE_MONITOR_EVENT_RENAMED ||
        event == G_FILE_MONITOR_EVENT_MOVED_IN ||
        event == G_FILE_MONITOR_EVENT_MOVED_OUT)
    {
        cached_at = 0;
    }
}


static void
clear_file_monitors (void)
{
    if (file_monitors == NULL) return;
    g_hash_table_destroy (file_monitors);
    file_monitors = NULL;
}


static void
sync_file_monitors (GPtrArray *desired_paths)
{
    if (file_monitors == NULL)
        file_monitors = g_hash_table_new_full (g_str_hash, g_str_equal,
                                               g_free, g_object_unref);

    /* Drop monitors whose path is no longer in the desired set. */
    GHashTableIter iter;
    gpointer key;
    g_hash_table_iter_init (&iter, file_monitors);
    while (g_hash_table_iter_next (&iter, &key, NULL))
    {
        gboolean wanted = FALSE;
        for (guint i = 0; i < desired_paths->len; i++)
        {
            if (g_strcmp0 (key, g_ptr_array_index (desired_paths, i)) == 0)
            {
                wanted = TRUE;
                break;
            }
        }
        if (!wanted)
            g_hash_table_iter_remove (&iter);
    }

    /* Add monitors for paths we don't already track. */
    for (guint i = 0; i < desired_paths->len; i++)
    {
        const gchar *path = g_ptr_array_index (desired_paths, i);
        if (path == NULL || g_hash_table_contains (file_monitors, path))
            continue;
        g_autoptr(GFile) f = g_file_new_for_path (path);
        g_autoptr(GError) err = NULL;
        GFileMonitor *m = g_file_monitor_file (f, G_FILE_MONITOR_NONE, NULL, &err);
        if (m == NULL)
        {
            if (err != NULL)
                g_warning ("Failed to monitor %s: %s", path, err->message);
            continue;
        }
        g_signal_connect (m, "changed", G_CALLBACK (on_db_file_changed), NULL);
        g_hash_table_insert (file_monitors, g_strdup (path), m);
    }
}


static GPtrArray *
load_entries_uncached (void)
{
    GPtrArray *entries = g_ptr_array_new_with_free_func ((GDestroyNotify) otp_search_entry_free);

    /* Collect the desired set of paths, then diff our current monitor set
     * against it — see sync_file_monitors. */
    g_autoptr (GPtrArray) desired_paths = g_ptr_array_new ();

    g_autoptr (GPtrArray) db_list = gsettings_common_get_db_list ();
    g_autofree gchar *fallback_path = NULL;
    if (db_list != NULL && db_list->len > 0)
    {
        for (guint i = 0; i < db_list->len; i++)
        {
            DbListEntry *dbe = g_ptr_array_index (db_list, i);
            if (dbe->path != NULL)
                g_ptr_array_add (desired_paths, dbe->path);
        }
    }
    else
    {
        fallback_path = gsettings_common_get_db_path ();
        if (fallback_path != NULL)
            g_ptr_array_add (desired_paths, fallback_path);
    }

    sync_file_monitors (desired_paths);

    if (db_list != NULL && db_list->len > 0)
    {
        for (guint i = 0; i < db_list->len; i++)
        {
            DbListEntry *dbe = g_ptr_array_index (db_list, i);
            load_entries_from_db (entries, dbe->path, dbe->name, i);
        }
    }
    else if (fallback_path != NULL)
    {
        load_entries_from_db (entries, fallback_path, NULL, 0);
    }

    return entries;
}


static GPtrArray *
get_entries (void)
{
    gint64 now = time (NULL);
    if (cached_entries != NULL && cached_at != 0 && (now - cached_at) < CACHE_TTL_SECONDS)
        return cached_entries;

    if (cached_entries != NULL)
        g_ptr_array_free (cached_entries, TRUE);

    cached_entries = load_entries_uncached ();
    cached_at = now;
    return cached_entries;
}


static gboolean
prewarm_cache (gpointer user_data G_GNUC_UNUSED)
{
    /* Run once on idle so the first user query doesn't pay the Argon2id
     * cost interactively. Failures are silently ignored — they'll be
     * retried on the next query. */
    (void) get_entries ();
    return G_SOURCE_REMOVE;
}


static gboolean
entry_matches_terms (const OtpSearchEntry *entry,
                     gchar               **terms,
                     gsize                 terms_len)
{
    if (terms_len == 0 || !entry->label_fold) return FALSE;
    for (gsize i = 0; i < terms_len; i++) {
        if (!terms[i]) continue;
        g_autofree gchar *t_fold = g_utf8_casefold (terms[i], -1);
        if (!g_strstr_len (entry->label_fold, -1, t_fold) &&
            !g_strstr_len (entry->issuer_fold, -1, t_fold))
            return FALSE;
    }
    return TRUE;
}


static gchar *
compute_otp_for_entry (const OtpSearchEntry *entry)
{
    /* Open, decrypt, look up the JSON object, compute the OTP, then wipe
     * everything. The Argon2id derivation pays the user-visible latency, but
     * the cached derived key in db_data->cached_derived_key + the file
     * monitor + the 60 s entry cache mean this only happens on the first Run
     * after the DB has changed. The trade vs caching the OTP value: heap
     * inspection of the daemon never reveals an active OTP. */
    if (entry == NULL || entry->db_path == NULL) return NULL;
    if (!gsettings_common_get_use_secret_service ()) return NULL;

    gchar *pwd = secret_password_lookup_sync (OTPCLIENT_SCHEMA, NULL, NULL,
                                               "string", entry->db_path, NULL);
    if (pwd == NULL) return NULL;

    DatabaseData *db_data = g_new0 (DatabaseData, 1);
    db_data->db_path = g_strdup (entry->db_path);
    db_data->key = secure_strdup (pwd);
    secret_password_free (pwd);
    db_data->max_file_size_from_memlock = global_max_file_size;

    GError *err = NULL;
    load_db (db_data, &err);
    gchar *otp = NULL;
    if (err == NULL && db_data->in_memory_json_data != NULL) {
        json_t *obj = json_array_get (db_data->in_memory_json_data, entry->json_index);
        if (obj != NULL)
            otp = get_entry_otp_value (obj);
    }
    if (err != NULL) g_clear_error (&err);

    db_invalidate_kdf_cache (db_data);
    if (db_data->key) gcry_free (db_data->key);
    if (db_data->in_memory_json_data) json_decref (db_data->in_memory_json_data);
    g_slist_free_full (db_data->objects_hash, g_free);
    g_free (db_data->db_path);
    g_free (db_data);

    return otp;
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
                otp = compute_otp_for_entry (e);
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
                otp = compute_otp_for_entry (e);
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
    g_idle_add (prewarm_cache, NULL);
    g_main_loop_run (main_loop);
    clear_file_monitors ();
    if (cached_entries != NULL) {
        g_ptr_array_free (cached_entries, TRUE);
        cached_entries = NULL;
    }
    g_main_loop_unref (main_loop);
    return 0;
}
