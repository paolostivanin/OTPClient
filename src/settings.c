#include <gtk/gtk.h>
#include "otpclient.h"
#include "message-dialogs.h"
#include "get-builder.h"

void
settings_dialog_cb (GSimpleAction *simple    __attribute__((unused)),
                    GVariant      *parameter __attribute__((unused)),
                    gpointer       user_data)
{
    AppData *app_data = (AppData *)user_data;

    GtkBuilder *builder = get_builder_from_partial_path (UI_PARTIAL_PATH);

    gchar *cfg_file_path;
#ifndef USE_FLATPAK_APP_FOLDER
    cfg_file_path = g_build_filename (g_get_home_dir (), ".config", "otpclient.cfg", NULL);
#else
    cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#endif
    GError *err = NULL;
    GKeyFile *kf = g_key_file_new ();
    if (!g_key_file_load_from_file (kf, cfg_file_path, G_KEY_FILE_NONE, &err)) {
        gchar *msg = g_strconcat ("Couldn't get data from config file: ", err->message, NULL);
        show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
        g_free (msg);
        g_free (cfg_file_path);
        g_key_file_free (kf);
        g_object_unref (builder);
        return;
    }
    g_free (cfg_file_path);
    
    // if key is not found, g_key_file_get_boolean returns FALSE and g_key_file_get_integer returns 0.
    // Therefore, having these values as default is exactly what we want. So no need to check whether or not the key is missing.
    app_data->show_next_otp = g_key_file_get_boolean (kf, "config", "show_next_otp", NULL);
    app_data->disable_notifications = g_key_file_get_boolean (kf, "config", "notifications", NULL);
    app_data->search_column = g_key_file_get_integer (kf, "config", "search_column", NULL);

    GtkWidget *dialog = GTK_WIDGET(gtk_builder_get_object (builder, "settings_diag_id"));
    GtkWidget *sno_switch = GTK_WIDGET(gtk_builder_get_object (builder, "nextotp_switch_id"));
    GtkWidget *dn_switch = GTK_WIDGET(gtk_builder_get_object (builder, "notif_switch_id"));
    GtkWidget *sc_cb = GTK_WIDGET(gtk_builder_get_object (builder, "search_by_cb_id"));

    gtk_switch_set_active (GTK_SWITCH(sno_switch), app_data->show_next_otp);
    gtk_switch_set_active (GTK_SWITCH(dn_switch), app_data->disable_notifications);
    gchar *active_id_string = g_strdup_printf ("%d", app_data->search_column);
    gtk_combo_box_set_active_id (GTK_COMBO_BOX(sc_cb), active_id_string);
    g_free (active_id_string);

    gtk_widget_show_all (dialog);

    switch (gtk_dialog_run (GTK_DIALOG(dialog))) {
        case GTK_RESPONSE_OK:
            app_data->show_next_otp = gtk_switch_get_active (GTK_SWITCH(sno_switch));
            app_data->disable_notifications = gtk_switch_get_active (GTK_SWITCH(dn_switch));
            app_data->search_column = (gint)g_ascii_strtoll (gtk_combo_box_get_active_id (GTK_COMBO_BOX(sc_cb)), NULL, 10);
            g_key_file_set_boolean (kf, "config", "show_next_otp", app_data->show_next_otp);
            g_key_file_set_boolean (kf, "config", "notifications", app_data->disable_notifications);
            g_key_file_set_integer (kf, "config", "search_column", app_data->search_column);
            break;
        case GTK_RESPONSE_CANCEL:
            break;
    }

    g_key_file_free (kf);

    gtk_widget_destroy (dialog);
    
    g_object_unref (builder);
}