#include <glib/gi18n.h>
#include "gui-misc.h"
#include "common.h"

static GSettings *
get_gsettings (void)
{
    GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
    if (source == NULL)
        return NULL;

    g_autoptr (GSettingsSchema) schema = g_settings_schema_source_lookup (
        source, "com.github.paolostivanin.OTPClient", TRUE);
    if (schema == NULL)
        return NULL;

    return g_settings_new ("com.github.paolostivanin.OTPClient");
}

gchar *
gui_misc_get_db_path_from_cfg (void)
{
    g_autoptr (GSettings) settings = get_gsettings ();
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
        g_autoptr (GSettings) s = get_gsettings ();
        if (s != NULL)
            g_settings_set_string (s, "db-path", db_path);
    }

    return db_path;
}

void
gui_misc_save_db_path_to_cfg (const gchar *db_path)
{
    g_autoptr (GSettings) settings = get_gsettings ();
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

void
gui_misc_send_notification (GApplication *app,
                            const gchar  *title,
                            const gchar  *body)
{
    g_autoptr (GNotification) notification = g_notification_new (title);
    g_notification_set_body (notification, body);
    g_application_send_notification (app, NULL, notification);
}
