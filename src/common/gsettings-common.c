#include "gsettings-common.h"
#include "common.h"

#define OTPCLIENT_SCHEMA_ID "com.github.paolostivanin.OTPClient"


void
db_list_entry_free (DbListEntry *entry)
{
    if (entry == NULL)
        return;
    g_free (entry->name);
    g_free (entry->path);
    g_free (entry);
}


GSettings *
gsettings_common_get_settings (void)
{
    GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
    if (source == NULL)
        return NULL;

    g_autoptr (GSettingsSchema) schema = g_settings_schema_source_lookup (
        source, OTPCLIENT_SCHEMA_ID, TRUE);
    if (schema == NULL)
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
    gboolean use_secret_service = TRUE;
    GKeyFile *kf = get_kf_ptr ();
    if (kf == NULL)
        return TRUE;

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
