#include <glib/gi18n.h>
#include "gui-misc.h"
#include "database-sidebar.h"
#include "common.h"
#include "gsettings-common.h"

gchar *
gui_misc_get_db_path_from_cfg (void)
{
    g_autoptr (GSettings) settings = gsettings_common_get_settings ();
    if (settings != NULL) {
        gchar *db_path = g_settings_get_string (settings, "db-path");
        if (db_path != NULL && db_path[0] != '\0')
            return db_path;
        g_free (db_path);
    }

    /* Fallback to GKeyFile for migration */
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

void
gui_misc_save_db_path_to_cfg (const gchar *db_path)
{
    g_autoptr (GSettings) settings = gsettings_common_get_settings ();
    if (settings != NULL)
        g_settings_set_string (settings, "db-path", db_path);

    /* Also save to GKeyFile for CLI/search-provider backwards compat */
    gchar *cfg_file_path;
#ifndef IS_FLATPAK
    cfg_file_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
#else
    cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#endif

    GKeyFile *kf = g_key_file_new ();
    g_key_file_load_from_file (kf, cfg_file_path, G_KEY_FILE_NONE, NULL);
    g_key_file_set_string (kf, "config", "db_path", db_path);

    GError *err = NULL;
    if (!g_key_file_save_to_file (kf, cfg_file_path, &err))
    {
        g_printerr ("Failed to save config: %s\n", err->message);
        g_clear_error (&err);
    }

    g_key_file_free (kf);
    g_free (cfg_file_path);
}

gchar *
gui_misc_derive_db_display_name (const gchar *path)
{
    g_autofree gchar *basename = g_path_get_basename (path);
    if (g_str_has_suffix (basename, ".enc")) {
        gsize len = strlen (basename);
        if (len > 4)
            return g_strndup (basename, len - 4);
    }
    return g_strdup (basename);
}

GPtrArray *
gui_misc_get_db_list (void)
{
    g_autoptr (GSettings) settings = gsettings_common_get_settings ();
    if (settings == NULL)
        return NULL;

    g_autoptr (GVariant) list = g_settings_get_value (settings, "db-list");
    gsize n_items = g_variant_n_children (list);

    if (n_items > 0) {
        GPtrArray *result = g_ptr_array_new_with_free_func (g_object_unref);
        GVariantIter iter;
        const gchar *name, *path;

        g_variant_iter_init (&iter, list);
        while (g_variant_iter_next (&iter, "(&s&s)", &name, &path))
            g_ptr_array_add (result, database_entry_new (name, path));

        return result;
    }

    /* v4 migration: db-list is empty but db-path might be set */
    gchar *db_path = gui_misc_get_db_path_from_cfg ();
    if (db_path == NULL || db_path[0] == '\0') {
        g_free (db_path);
        return NULL;
    }

    g_autofree gchar *display_name = gui_misc_derive_db_display_name (db_path);

    GPtrArray *result = g_ptr_array_new_with_free_func (g_object_unref);
    g_ptr_array_add (result, database_entry_new (display_name, db_path));
    g_free (db_path);

    /* Persist the migrated entry */
    GVariantBuilder builder;
    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ss)"));
    g_variant_builder_add (&builder, "(ss)", display_name,
                           database_entry_get_path (g_ptr_array_index (result, 0)));
    g_settings_set_value (settings, "db-list", g_variant_builder_end (&builder));

    return result;
}

void
gui_misc_save_db_list (GListStore *db_store)
{
    g_autoptr (GSettings) settings = gsettings_common_get_settings ();
    if (settings == NULL)
        return;

    GVariantBuilder builder;
    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ss)"));

    guint n = g_list_model_get_n_items (G_LIST_MODEL (db_store));
    for (guint i = 0; i < n; i++) {
        g_autoptr (DatabaseEntry) entry = g_list_model_get_item (G_LIST_MODEL (db_store), i);
        g_variant_builder_add (&builder, "(ss)",
                               database_entry_get_name (entry),
                               database_entry_get_path (entry));
    }

    g_settings_set_value (settings, "db-list", g_variant_builder_end (&builder));
}

gboolean
gui_misc_add_db_to_list (GListStore  *db_store,
                         const gchar *name,
                         const gchar *path)
{
    guint n = g_list_model_get_n_items (G_LIST_MODEL (db_store));
    for (guint i = 0; i < n; i++) {
        g_autoptr (DatabaseEntry) entry = g_list_model_get_item (G_LIST_MODEL (db_store), i);
        if (g_strcmp0 (database_entry_get_path (entry), path) == 0)
            return FALSE;
    }

    DatabaseEntry *entry = database_entry_new (name, path);
    g_list_store_append (db_store, entry);
    g_object_unref (entry);

    gui_misc_save_db_list (db_store);
    return TRUE;
}

void
gui_misc_remove_db_from_list (GListStore *db_store,
                              guint       index)
{
    guint n = g_list_model_get_n_items (G_LIST_MODEL (db_store));
    g_return_if_fail (index < n);

    g_list_store_remove (db_store, index);
    gui_misc_save_db_list (db_store);
}

void
gui_misc_rename_db_in_list (GListStore  *db_store,
                            guint        index,
                            const gchar *new_name)
{
    guint n = g_list_model_get_n_items (G_LIST_MODEL (db_store));
    g_return_if_fail (index < n);

    g_autoptr (DatabaseEntry) entry = g_list_model_get_item (G_LIST_MODEL (db_store), index);

    database_entry_set_name (entry, new_name);
    gui_misc_save_db_list (db_store);
}

void
gui_misc_send_notification (GApplication *app,
                            const gchar  *title,
                            const gchar  *body)
{
    g_autoptr (GNotification) notification = g_notification_new (title);
    g_notification_set_body (notification, body);
    g_application_send_notification (app, NULL, notification);
}
