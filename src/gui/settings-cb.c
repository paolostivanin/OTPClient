#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include <libsecret/secret.h>
#include "message-dialogs.h"
#include "get-builder.h"
#include "../common/secret-schema.h"
#include "gui-misc.h"
#include "../common/macros.h"
#ifdef ENABLE_MINIMIZE_TO_TRAY
#include "tray.h"
#endif

typedef struct settings_data_t {
    GtkWidget *dss_switch;
    GtkWidget *tray_switch;
    GtkWidget *al_switch;
    GtkWidget *inactivity_cb;
    GtkWidget *validity_switch;
    GtkWidget *validity_color_btn;
    GtkWidget *validity_warning_color_btn;
    GtkWidget *search_provider_switch;
    AppData *app_data;
} SettingsData;

static void     handle_al_ss                  (AppData *app_data,
                                               GtkWidget *al_switch,
                                               GtkWidget *inactivity_cb,
                                               GtkWidget *dss_switch);

static gboolean handle_secretservice_switch   (GtkSwitch   *sw,
                                               gboolean     state,
                                               gpointer     user_data);

static void     handle_secretservice_combobox (GtkComboBox *cb,
                                               gpointer     user_data);

static gboolean handle_autolock               (GtkSwitch   *sw,
                                               gboolean     state,
                                               gpointer     user_data);

static gboolean handle_tray_switch            (GtkSwitch   *sw,
                                               gboolean     state,
                                               gpointer     user_data);

static gboolean handle_validity_switch        (GtkSwitch   *sw,
                                               gboolean     state,
                                               gpointer     user_data);


void
settings_dialog_cb (GSimpleAction *simple UNUSED,
                    GVariant      *parameter UNUSED,
                    gpointer       user_data)
{
    CAST_USER_DATA(AppData, app_data, user_data);
    SettingsData *settings_data = g_new0 (SettingsData, 1);
    settings_data->app_data = app_data;

    gchar *cfg_file_path;
#ifndef IS_FLATPAK
    cfg_file_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
#else
    cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#endif
    GError *err = NULL;
    GKeyFile *kf = g_key_file_new ();
    if (!g_key_file_load_from_file (kf, cfg_file_path, G_KEY_FILE_NONE, &err)) {
        // if config file is not set, we use the default values.
        g_clear_error (&err);
        g_key_file_set_boolean (kf, "config", "show_next_otp", app_data->show_next_otp);
        g_key_file_set_boolean (kf, "config", "notifications", app_data->disable_notifications);
        g_key_file_set_boolean (kf, "config", "show_validity_seconds", app_data->show_validity_seconds);
        gchar *validity_color = gdk_rgba_to_string (&app_data->validity_color);
        gchar *warning_color = gdk_rgba_to_string (&app_data->validity_warning_color);
        g_key_file_set_string (kf, "config", "validity_color", validity_color);
        g_key_file_set_string (kf, "config", "validity_warning_color", warning_color);
        g_free (validity_color);
        g_free (warning_color);
        g_key_file_set_boolean (kf, "config", "auto_lock", app_data->auto_lock);
        g_key_file_set_integer (kf, "config", "inactivity_timeout", app_data->inactivity_timeout);
        g_key_file_set_boolean (kf, "config", "dark_theme", app_data->use_dark_theme);
        g_key_file_set_boolean (kf, "config", "use_secret_service", app_data->use_secret_service);
        g_key_file_set_boolean (kf, "config", "use_tray", app_data->use_tray);
        g_key_file_set_boolean (kf, "config", "search_provider_enabled", app_data->search_provider_enabled);
        if (!g_key_file_save_to_file (kf, cfg_file_path, &err)) {
            gchar *msg = g_strconcat (_("Couldn't save default settings: "), err->message, NULL);
            show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_WARNING);
            g_free (msg);
            g_clear_error (&err);
        }
    } else {
        // if key is not found, g_key_file_get_boolean returns FALSE and g_key_file_get_integer returns 0.
        // Therefore, having these values as default is exactly what we want. So no need to check whether the key is missing.
        app_data->show_next_otp = g_key_file_get_boolean (kf, "config", "show_next_otp", NULL);
        app_data->disable_notifications = g_key_file_get_boolean (kf, "config", "notifications", NULL);
        app_data->show_validity_seconds = g_key_file_get_boolean (kf, "config", "show_validity_seconds", NULL);
        gchar *validity_color = g_key_file_get_string (kf, "config", "validity_color", NULL);
        gchar *warning_color = g_key_file_get_string (kf, "config", "validity_warning_color", NULL);
        if (validity_color != NULL) {
            gdk_rgba_parse (&app_data->validity_color, validity_color);
        }
        if (warning_color != NULL) {
            gdk_rgba_parse (&app_data->validity_warning_color, warning_color);
        }
        g_free (validity_color);
        g_free (warning_color);
        app_data->auto_lock = g_key_file_get_boolean (kf, "config", "auto_lock", NULL);
        app_data->inactivity_timeout = g_key_file_get_integer (kf, "config", "inactivity_timeout", NULL);
        app_data->use_dark_theme = g_key_file_get_boolean (kf, "config", "dark_theme", NULL);
        app_data->use_secret_service = g_key_file_get_boolean (kf, "config", "use_secret_service", &err);
        app_data->use_tray = g_key_file_get_boolean (kf, "config", "use_tray", NULL);
        GError *search_err = NULL;
        app_data->search_provider_enabled = g_key_file_get_boolean (kf, "config", "search_provider_enabled", &search_err);
        if (search_err != NULL && g_error_matches (search_err, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND)) {
            app_data->search_provider_enabled = TRUE;
            g_clear_error (&search_err);
        }
        if (search_err != NULL) {
            g_clear_error (&search_err);
        }
        if (err != NULL && g_error_matches (err, G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_KEY_NOT_FOUND)) {
            // if the key is not found, we set it to TRUE and save it to the config file.
            app_data->use_secret_service = TRUE;
            g_clear_error (&err);
        }
    }

    GtkBuilder *builder = get_builder_from_partial_path(UI_PARTIAL_PATH);
    GtkWidget *dialog = GTK_WIDGET(gtk_builder_get_object (builder, "settings_diag_id"));
    GtkWidget *sno_switch = GTK_WIDGET(gtk_builder_get_object (builder, "nextotp_switch_id"));
    GtkWidget *dn_switch = GTK_WIDGET(gtk_builder_get_object (builder, "notif_switch_id"));
    settings_data->validity_switch = GTK_WIDGET(gtk_builder_get_object (builder, "validity_seconds_switch_id"));
    settings_data->validity_color_btn = GTK_WIDGET(gtk_builder_get_object (builder, "validity_color_btn_id"));
    settings_data->validity_warning_color_btn = GTK_WIDGET(gtk_builder_get_object (builder, "validity_warning_color_btn_id"));
    g_signal_connect (settings_data->validity_switch, "state-set", G_CALLBACK(handle_validity_switch), settings_data);
    settings_data->al_switch = GTK_WIDGET(gtk_builder_get_object (builder, "autolock_switch_id"));
    g_signal_connect (settings_data->al_switch, "state-set", G_CALLBACK(handle_secretservice_switch), settings_data);
    settings_data->inactivity_cb = GTK_WIDGET(gtk_builder_get_object (builder, "autolock_inactive_cb_id"));
    g_signal_connect (settings_data->inactivity_cb, "changed", G_CALLBACK(handle_secretservice_combobox), settings_data);
    GtkWidget *dt_switch = GTK_WIDGET(gtk_builder_get_object (builder, "dark_theme_switch_id"));
    settings_data->dss_switch = GTK_WIDGET(gtk_builder_get_object (builder, "secret_service_switch_id"));
    g_signal_connect (settings_data->dss_switch, "state-set", G_CALLBACK(handle_autolock), settings_data);
    settings_data->tray_switch = GTK_WIDGET(gtk_builder_get_object (builder, "tray_switch_id"));
    settings_data->search_provider_switch = GTK_WIDGET(gtk_builder_get_object (builder, "search_provider_switch_id"));
    #ifdef ENABLE_MINIMIZE_TO_TRAY
    g_signal_connect (settings_data->tray_switch, "state-set", G_CALLBACK(handle_tray_switch), settings_data);
    #else
    GtkGrid *grid = GTK_GRID(gtk_builder_get_object(builder, "grid_settings"));
    gtk_grid_remove_row(grid, 9);  // we cannot control visibility of the child eledments
    #endif

    gtk_window_set_transient_for (GTK_WINDOW(dialog), GTK_WINDOW(app_data->main_window));

    gtk_switch_set_active (GTK_SWITCH(sno_switch), app_data->show_next_otp);
    gtk_switch_set_active (GTK_SWITCH(dn_switch), app_data->disable_notifications);
    gtk_switch_set_active (GTK_SWITCH(settings_data->validity_switch), app_data->show_validity_seconds);
    gtk_color_chooser_set_rgba (GTK_COLOR_CHOOSER(settings_data->validity_color_btn), &app_data->validity_color);
    gtk_color_chooser_set_rgba (GTK_COLOR_CHOOSER(settings_data->validity_warning_color_btn), &app_data->validity_warning_color);
    handle_validity_switch (GTK_SWITCH(settings_data->validity_switch), app_data->show_validity_seconds, settings_data);
    gtk_switch_set_active (GTK_SWITCH(settings_data->al_switch), app_data->auto_lock);
    gtk_switch_set_active (GTK_SWITCH(dt_switch), app_data->use_dark_theme);
    gtk_switch_set_active (GTK_SWITCH(settings_data->dss_switch), app_data->use_secret_service);
    gtk_switch_set_active (GTK_SWITCH(settings_data->tray_switch), app_data->use_tray);
    gtk_switch_set_active (GTK_SWITCH(settings_data->search_provider_switch), app_data->search_provider_enabled);
    gchar *active_id_string = g_strdup_printf ("%d", app_data->inactivity_timeout);
    gtk_combo_box_set_active_id (GTK_COMBO_BOX(settings_data->inactivity_cb), active_id_string);
    g_free (active_id_string);

    handle_al_ss (app_data, settings_data->al_switch, settings_data->inactivity_cb, settings_data->dss_switch);

    gtk_widget_show_all (dialog);

    gboolean old_ss_value = app_data->use_secret_service;
    gboolean old_validity_seconds = app_data->show_validity_seconds;
    GdkRGBA old_validity_color = app_data->validity_color;
    GdkRGBA old_validity_warning_color = app_data->validity_warning_color;
    switch (gtk_dialog_run (GTK_DIALOG(dialog))) {
        case GTK_RESPONSE_OK:
            app_data->show_next_otp = gtk_switch_get_active (GTK_SWITCH(sno_switch));
            app_data->disable_notifications = gtk_switch_get_active (GTK_SWITCH(dn_switch));
            app_data->show_validity_seconds = gtk_switch_get_active (GTK_SWITCH(settings_data->validity_switch));
            gtk_color_chooser_get_rgba (GTK_COLOR_CHOOSER(settings_data->validity_color_btn), &app_data->validity_color);
            gtk_color_chooser_get_rgba (GTK_COLOR_CHOOSER(settings_data->validity_warning_color_btn), &app_data->validity_warning_color);
            app_data->auto_lock = gtk_switch_get_active (GTK_SWITCH(settings_data->al_switch));
            app_data->inactivity_timeout = (gint)g_ascii_strtoll (gtk_combo_box_get_active_id (GTK_COMBO_BOX(settings_data->inactivity_cb)), NULL, 10);
            app_data->use_dark_theme = gtk_switch_get_active (GTK_SWITCH(dt_switch));
            app_data->use_secret_service = gtk_switch_get_active (GTK_SWITCH(settings_data->dss_switch));
            app_data->use_tray = gtk_switch_get_active (GTK_SWITCH(settings_data->tray_switch));
            app_data->search_provider_enabled = gtk_switch_get_active (GTK_SWITCH(settings_data->search_provider_switch));
            g_key_file_set_boolean (kf, "config", "show_next_otp", app_data->show_next_otp);
            g_key_file_set_boolean (kf, "config", "notifications", app_data->disable_notifications);
            g_key_file_set_boolean (kf, "config", "show_validity_seconds", app_data->show_validity_seconds);
            gchar *validity_color = gdk_rgba_to_string (&app_data->validity_color);
            gchar *warning_color = gdk_rgba_to_string (&app_data->validity_warning_color);
            g_key_file_set_string (kf, "config", "validity_color", validity_color);
            g_key_file_set_string (kf, "config", "validity_warning_color", warning_color);
            g_free (validity_color);
            g_free (warning_color);
            g_key_file_set_boolean (kf, "config", "auto_lock", app_data->auto_lock);
            g_key_file_set_integer (kf, "config", "inactivity_timeout", app_data->inactivity_timeout);
            g_key_file_set_boolean (kf, "config", "dark_theme", app_data->use_dark_theme);
            g_key_file_set_boolean (kf, "config", "use_secret_service", app_data->use_secret_service);
            g_key_file_set_boolean (kf, "config", "use_tray", app_data->use_tray);
            g_key_file_set_boolean (kf, "config", "search_provider_enabled", app_data->search_provider_enabled);
            if (old_ss_value == TRUE && app_data->use_secret_service == FALSE) {
                // secret service was just disabled, so we have to clear the password from the keyring
                secret_password_clear (OTPCLIENT_SCHEMA, NULL, on_password_cleared, NULL, "string", "main_pwd", NULL);
            }
            if (!g_key_file_save_to_file (kf, cfg_file_path, NULL)) {
                g_printerr ("%s\n", _("Error while saving the config file."));
            }
            gboolean colors_changed = !gdk_rgba_equal (&old_validity_color, &app_data->validity_color)
                || !gdk_rgba_equal (&old_validity_warning_color, &app_data->validity_warning_color);
            if ((old_validity_seconds != app_data->show_validity_seconds || colors_changed) && app_data->tree_view != NULL) {
                gtk_widget_queue_draw (GTK_WIDGET (app_data->tree_view));
            }
            if (app_data->filter_model != NULL) {
                gtk_tree_model_filter_refilter (app_data->filter_model);
            }
            break;
        case GTK_RESPONSE_CANCEL:
            break;
    }

    g_free (cfg_file_path);
    g_key_file_free (kf);
    g_free (settings_data);

    gtk_widget_destroy (dialog);

    g_object_unref (builder);
}


void
show_settings_cb_shortcut (GtkWidget *w UNUSED,
                           gpointer   user_data)
{
    settings_dialog_cb (NULL, NULL, user_data);
}


static void
handle_al_ss (AppData   *app_data,
              GtkWidget *al_switch,
              GtkWidget *inactivity_cb,
              GtkWidget *dss_switch)
{
    GKeyFile *kf = get_kf_ptr ();
    if (app_data->use_secret_service == TRUE) {
        // secret service is enabled, so we need to disable auto-lock
        app_data->auto_lock = FALSE;
        app_data->inactivity_timeout = 0;
        gtk_widget_set_sensitive (al_switch, FALSE);
        gtk_widget_set_sensitive (inactivity_cb, FALSE);
    } else {
        if (app_data->auto_lock == TRUE || app_data->inactivity_timeout > 0) {
            // if secret service is disabled AND (auto-lock is enabled OR timeout is enabled), we need to disable secret service
            app_data->use_secret_service = FALSE;
            gtk_widget_set_sensitive (dss_switch, FALSE);
        }
    }
    if (kf != NULL) {
        // Until the migration is done for all users, we need to manually update the settings.
        // This code block can be removed once all distros have upgrade to, at least, version 3.1.4.
        g_key_file_set_boolean (kf, "config", "auto_lock", app_data->auto_lock);
        g_key_file_set_boolean (kf, "config", "use_secret_service", app_data->use_secret_service);
        g_key_file_set_integer (kf, "config", "inactivity_timeout", app_data->inactivity_timeout);
        gchar *cfg_file_path;
#ifndef IS_FLATPAK
        cfg_file_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
#else
        cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#endif
        if (!g_key_file_save_to_file (kf, cfg_file_path, NULL)) {
            g_printerr ("%s\n", _("Error while saving the config file."));
        }
        g_key_file_free (kf);
    }
}


static gboolean
handle_secretservice_switch (GtkSwitch *sw,
                             gboolean   state,
                             gpointer   user_data)
{
   /* SecretService is disabled (TRUE), and we disable both autolock (FALSE) AND autolock timeout (0):
    *  - secret_service_switch_id must be set to sensitive
    */
    CAST_USER_DATA(SettingsData, settings_data, user_data);
    settings_data->app_data->auto_lock = state;
    if (state == FALSE && settings_data->app_data->inactivity_timeout == 0) {
        gtk_widget_set_sensitive (settings_data->dss_switch, TRUE);
    } else {
        gtk_widget_set_sensitive (settings_data->dss_switch, FALSE);
    }
    gtk_switch_set_state (GTK_SWITCH(sw), state);

    return TRUE;
}


static void
handle_secretservice_combobox (GtkComboBox *cb,
                               gpointer     user_data)
{
   /* SecretService is disabled (TRUE), and we disable both autolock (FALSE) AND autolock timeout (0):
    *  - secret_service_switch_id must be set to sensitive
    */
    CAST_USER_DATA(SettingsData, settings_data, user_data);
    settings_data->app_data->inactivity_timeout = (gint)g_ascii_strtoll (gtk_combo_box_get_active_id (GTK_COMBO_BOX(cb)), NULL, 10);
    if (settings_data->app_data->inactivity_timeout == 0 && settings_data->app_data->auto_lock == FALSE) {
        gtk_widget_set_sensitive (settings_data->dss_switch, TRUE);
    } else {
        gtk_widget_set_sensitive (settings_data->dss_switch, FALSE);
    }
}


static gboolean
handle_autolock (GtkSwitch *sw UNUSED,
                 gboolean   state,
                 gpointer   user_data)
{
   /* SecretService is enabled, and we disable it (TRUE -> FALSE):
    *  - lock_btn_id, autolock_switch_id and autolock_inactive_cb_id must be set to sensitive
    *  - add entry signal ctrl-l
    */
    CAST_USER_DATA(SettingsData, settings_data, user_data);
    if (state == FALSE) {
        gtk_widget_set_sensitive (GTK_WIDGET(gtk_builder_get_object (settings_data->app_data->builder, "lock_btn_id")), TRUE);
        gtk_widget_set_sensitive (settings_data->al_switch, TRUE);
        gtk_widget_set_sensitive (settings_data->inactivity_cb, TRUE);
        gtk_binding_entry_add_signal (gtk_binding_set_by_class (GTK_APPLICATION_WINDOW_GET_CLASS(settings_data->app_data->main_window)), GDK_KEY_l, GDK_CONTROL_MASK, "lock-app", 0);
    } else {
        gtk_widget_set_sensitive (GTK_WIDGET(gtk_builder_get_object (settings_data->app_data->builder, "lock_btn_id")), FALSE);
        gtk_widget_set_sensitive (settings_data->al_switch, FALSE);
        gtk_widget_set_sensitive (settings_data->inactivity_cb, FALSE);
        gtk_binding_entry_remove (gtk_binding_set_by_class (GTK_APPLICATION_WINDOW_GET_CLASS(settings_data->app_data->main_window)), GDK_KEY_l, GDK_CONTROL_MASK);
    }
    gtk_switch_set_state (GTK_SWITCH(sw), state);

    return TRUE;
}

#ifdef ENABLE_MINIMIZE_TO_TRAY
static gboolean
handle_tray_switch (GtkSwitch *sw UNUSED,
                    gboolean   state,
                    gpointer   user_data)
{
    CAST_USER_DATA(SettingsData, settings_data, user_data);
    settings_data->app_data->use_tray = state;
    switch_tray_use (settings_data->app_data);
    gtk_switch_set_state (GTK_SWITCH(sw), state);

    return TRUE;
}
#endif

static gboolean
handle_validity_switch (GtkSwitch *sw,
                        gboolean   state,
                        gpointer   user_data)
{
    CAST_USER_DATA(SettingsData, settings_data, user_data);
    gtk_widget_set_sensitive (settings_data->validity_color_btn, !state);
    gtk_widget_set_sensitive (settings_data->validity_warning_color_btn, !state);
    gtk_switch_set_state (sw, state);

    return TRUE;
}
