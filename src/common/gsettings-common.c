#include "gsettings-common.h"
#include "common.h"

#define OTPCLIENT_SCHEMA_ID "com.github.paolostivanin.OTPClient"

gint64
gsettings_common_get_database_time (GSettings *settings, const gchar *key, const gchar *path)
{
    if (settings == NULL || path == NULL) return 0;
    g_autofree gchar *normalized = g_canonicalize_filename (path, NULL);
    g_autoptr (GVariant) value = g_settings_get_value (settings, key);
    gint64 timestamp = 0;
    g_variant_lookup (value, normalized, "x", &timestamp);
    return MAX (timestamp, 0);
}

void
gsettings_common_set_database_time (GSettings *settings, const gchar *key,
                                    const gchar *path, gint64 timestamp)
{
    if (settings == NULL || path == NULL) return;
    g_autofree gchar *normalized = g_canonicalize_filename (path, NULL);
    g_autoptr (GVariant) old = g_settings_get_value (settings, key);
    GVariantBuilder builder;
    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sx}"));
    GVariantIter iter;
    const gchar *entry;
    gint64 time;
    g_variant_iter_init (&iter, old);
    while (g_variant_iter_next (&iter, "{&sx}", &entry, &time)) {
        if (!g_str_equal (entry, normalized))
            g_variant_builder_add (&builder, "{sx}", entry, time);
    }
    if (timestamp > 0)
        g_variant_builder_add (&builder, "{sx}", normalized, timestamp);
    g_settings_set_value (settings, key, g_variant_builder_end (&builder));
}

void
gsettings_common_relocate_backup_history (const gchar *old_path, const gchar *new_path)
{
    g_autofree gchar *old = g_canonicalize_filename (old_path, NULL);
    g_autofree gchar *next = g_canonicalize_filename (new_path, NULL);
    if (g_str_equal (old, next)) return;
    g_autoptr (GSettings) settings = gsettings_common_get_settings ();
    const gchar *keys[] = {OTPCLIENT_BACKUP_TIMES, OTPCLIENT_BACKUP_SNOOZES};
    for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
        gint64 timestamp = MAX (gsettings_common_get_database_time (settings, keys[i], old),
                                gsettings_common_get_database_time (settings, keys[i], next));
        gsettings_common_set_database_time (settings, keys[i], next, timestamp);
        gsettings_common_set_database_time (settings, keys[i], old, 0);
    }
}


void
db_list_entry_free (DbListEntry *entry)
{
    if (entry == NULL)
        return;
    g_free (entry->name);
    g_free (entry->path);
    g_free (entry);
}


/* Every key in data/com.github.paolostivanin.OTPClient.gschema.xml. GSettings
 * aborts the process on a read or write of a key the schema does not have, so
 * a compiled schema left over from an older release, found by GLib ahead of the
 * current one, used to kill the app at startup (#475). The settings import test
 * keeps this list identical to the schema. */
static const gchar *const schema_keys[] = {
    "db-path",
    "db-list",
    "show-next-otp",
    "notification-enabled",
    "search-column",
    "window-width",
    "window-height",
    "dark-theme",
    "auto-lock",
    "auto-lock-timeout",
    "secret-service",
    "secret-service-v4-migrated",
    "search-provider-enabled",
    "search-provider-keyword",
    "show-validity-seconds",
    "validity-color",
    "validity-warning-color",
    "show-sidebar",
    "clipboard-clear-timeout",
    "minimize-to-tray",
    "start-minimized",
    "tray-icon-style",
    "autostart",
    "startup-reconcile-pending",
    "last-seen-version",
    OTPCLIENT_BACKUP_TIMES,
    OTPCLIENT_BACKUP_SNOOZES,
    "hide-otps",
    NULL
};


const gchar *const *
gsettings_common_get_schema_keys (void)
{
    return schema_keys;
}


GStrv
gsettings_common_get_missing_keys (GSettingsSchema *schema)
{
    g_return_val_if_fail (schema != NULL, NULL);

    GStrvBuilder *builder = g_strv_builder_new ();
    for (gsize i = 0; schema_keys[i] != NULL; i++) {
        if (!g_settings_schema_has_key (schema, schema_keys[i]))
            g_strv_builder_add (builder, schema_keys[i]);
    }
    GStrv missing = g_strv_builder_end (builder);
    g_strv_builder_unref (builder);
    return missing;
}


static gboolean
dir_holds_schema (const gchar *dir)
{
    g_autoptr (GSettingsSchemaSource) source =
        g_settings_schema_source_new_from_directory (dir, NULL, FALSE, NULL);
    if (source == NULL)
        return FALSE;
    g_autoptr (GSettingsSchema) schema =
        g_settings_schema_source_lookup (source, OTPCLIENT_SCHEMA_ID, FALSE);
    return schema != NULL;
}


/* The directory GLib took the schema from, searched in GLib's own order, or
 * NULL when the stale copy cannot be pinned down. Only for the warning. */
static gchar *
find_schema_dir (void)
{
    const gchar *env = g_getenv ("GSETTINGS_SCHEMA_DIR");
    if (env != NULL) {
        g_auto (GStrv) dirs = g_strsplit (env, G_SEARCHPATH_SEPARATOR_S, 0);
        for (gsize i = 0; dirs[i] != NULL; i++) {
            if (dirs[i][0] != '\0' && dir_holds_schema (dirs[i]))
                return g_strdup (dirs[i]);
        }
    }

    g_autofree gchar *user_dir = g_build_filename (g_get_user_data_dir (), "glib-2.0", "schemas", NULL);
    if (dir_holds_schema (user_dir))
        return g_steal_pointer (&user_dir);

    const gchar *const *data_dirs = g_get_system_data_dirs ();
    for (gsize i = 0; data_dirs[i] != NULL; i++) {
        g_autofree gchar *dir = g_build_filename (data_dirs[i], "glib-2.0", "schemas", NULL);
        if (dir_holds_schema (dir))
            return g_steal_pointer (&dir);
    }
    return NULL;
}


static gboolean
installed_schema_is_usable (void)
{
    GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
    if (source == NULL)
        return FALSE;

    g_autoptr (GSettingsSchema) schema = g_settings_schema_source_lookup (
        source, OTPCLIENT_SCHEMA_ID, TRUE);
    if (schema == NULL)
        return FALSE;

    g_auto (GStrv) missing = gsettings_common_get_missing_keys (schema);
    if (missing[0] == NULL)
        return TRUE;

    /* Treat it like a missing schema: every caller copes with NULL, and the
     * defaults are better than an abort. Settings stay unsaved until the
     * install is fixed, so say how. */
    g_autofree gchar *keys = g_strjoinv (", ", missing);
    g_autofree gchar *dir = find_schema_dir ();
    g_warning ("The settings schema %s%s%s is older than this build of OTPClient "
               "and lacks: %s. Settings will be neither loaded nor saved. Remove or "
               "update that copy of %s.gschema.xml and run glib-compile-schemas "
               "on its directory, or reinstall OTPClient.",
               OTPCLIENT_SCHEMA_ID, dir != NULL ? " in " : "", dir != NULL ? dir : "",
               keys, OTPCLIENT_SCHEMA_ID);
    return FALSE;
}


GSettings *
gsettings_common_get_settings (void)
{
    /* GLib builds its default schema source once per process, so the answer,
     * and the warning, only need to happen once too. */
    static gsize usable = 0;
    if (g_once_init_enter (&usable))
        g_once_init_leave (&usable, installed_schema_is_usable () ? 1 : 2);

    if (usable != 1)
        return NULL;

    return g_settings_new (OTPCLIENT_SCHEMA_ID);
}


gchar *
gsettings_common_get_db_path (void)
{
    g_autoptr (GSettings) settings = gsettings_common_get_settings ();
    if (settings != NULL) {
        gchar *db_path = g_settings_get_string (settings, "db-path");
        if (db_path != NULL && db_path[0] != '\0')
            return db_path;
        g_free (db_path);
    }

    /* Fallback to GKeyFile */
    GKeyFile *kf = get_kf_ptr ();
    if (kf == NULL)
        return NULL;

    gchar *db_path = g_key_file_get_string (kf, "config", "db_path", NULL);
    g_key_file_free (kf);

    /* Migrate to GSettings if we found a path */
    if (db_path != NULL && db_path[0] != '\0') {
        g_autoptr (GSettings) s = gsettings_common_get_settings ();
        if (s != NULL)
            g_settings_set_string (s, "db-path", db_path);
    }

    return db_path;
}


gboolean
gsettings_common_get_use_secret_service (void)
{
    g_autoptr (GSettings) settings = gsettings_common_get_settings ();
    if (settings != NULL)
        return g_settings_get_boolean (settings, "secret-service");

    /* Fallback to GKeyFile */
    gboolean use_secret_service = FALSE;
    GKeyFile *kf = get_kf_ptr ();
    if (kf == NULL)
        return FALSE;

    use_secret_service = g_key_file_get_boolean (kf, "config", "use_secret_service", NULL);
    g_key_file_free (kf);

    return use_secret_service;
}


gboolean
gsettings_common_get_search_provider_enabled (void)
{
    g_autoptr (GSettings) settings = gsettings_common_get_settings ();
    if (settings != NULL)
        return g_settings_get_boolean (settings, "search-provider-enabled");

    /* Fallback to GKeyFile */
    gboolean enabled = TRUE;
    GKeyFile *kf = get_kf_ptr ();
    if (kf == NULL)
        return TRUE;

    GError *err = NULL;
    enabled = g_key_file_get_boolean (kf, "config", "search_provider_enabled", &err);
    if (err != NULL) {
        enabled = TRUE;
        g_error_free (err);
    }
    g_key_file_free (kf);

    return enabled;
}


gchar *
gsettings_common_get_search_provider_keyword (void)
{
    g_autoptr (GSettings) settings = gsettings_common_get_settings ();
    if (settings != NULL)
        return g_settings_get_string (settings, "search-provider-keyword");

    return g_strdup ("otp");
}


GPtrArray *
gsettings_common_get_db_list (void)
{
    g_autoptr (GSettings) settings = gsettings_common_get_settings ();
    if (settings == NULL)
        return NULL;

    g_autoptr (GVariant) list = g_settings_get_value (settings, "db-list");
    gsize n_items = g_variant_n_children (list);

    if (n_items == 0)
        return NULL;

    GPtrArray *result = g_ptr_array_new_with_free_func ((GDestroyNotify) db_list_entry_free);
    GVariantIter iter;
    const gchar *name, *path;

    g_variant_iter_init (&iter, list);
    while (g_variant_iter_next (&iter, "(&s&s)", &name, &path)) {
        DbListEntry *entry = g_new0 (DbListEntry, 1);
        entry->name = g_strdup (name);
        entry->path = g_strdup (path);
        g_ptr_array_add (result, entry);
    }

    return result;
}
