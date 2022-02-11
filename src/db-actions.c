#include <gtk/gtk.h>
#include "data.h"
#include "message-dialogs.h"
#include "db-actions.h"

void
select_file_icon_pressed_cb (GtkEntry         *entry,
                             gint              position __attribute__((unused)),
                             GdkEventButton   *event    __attribute__((unused)),
                             gpointer          data)
{
    AppData *app_data = (AppData *)data;

    gint action_int = GPOINTER_TO_INT(g_object_get_data (G_OBJECT(entry), "action"));
    GtkFileChooserAction action = (action_int == ACTION_OPEN) ? GTK_FILE_CHOOSER_ACTION_OPEN : GTK_FILE_CHOOSER_ACTION_SAVE;

#if GTK_CHECK_VERSION(3, 20, 0)
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new ("Select database",
                                                                GTK_WINDOW(app_data->main_window),
                                                                action,
                                                                "OK",
                                                                "Cancel");

    gint res = gtk_native_dialog_run (GTK_NATIVE_DIALOG(dialog));
#else
    GtkWidget *dialog = gtk_file_chooser_dialog_new ("Select database",
                                                     GTK_WINDOW(app_data->main_window),
                                                     action,
                                                     "Cancel", GTK_RESPONSE_CANCEL,
                                                     "OK", GTK_RESPONSE_ACCEPT,
                                                     NULL);

    gint res = gtk_dialog_run (GTK_DIALOG(dialog));
#endif
    if (res == GTK_RESPONSE_ACCEPT) {
        gtk_entry_set_text (entry, gtk_file_chooser_get_filename (GTK_FILE_CHOOSER(dialog)));
    }
#if GTK_CHECK_VERSION(3, 20, 0)
    g_object_unref (dialog);
#else
    gtk_widget_destroy (dialog);
#endif
}


void
update_cfg_file (AppData *app_data)
{
    GError *cfg_err = NULL;
    gchar *msg = NULL;
    GKeyFile *kf = g_key_file_new ();
    gchar *cfg_file_path;
#ifndef USE_FLATPAK_APP_FOLDER
    cfg_file_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
#else
    cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#endif
    g_key_file_load_from_file (kf, cfg_file_path, G_KEY_FILE_NONE, NULL);
    g_key_file_set_string (kf, "config", "db_path", app_data->db_data->db_path);
    g_key_file_save_to_file (kf, cfg_file_path, &cfg_err);
    if (cfg_err != NULL) {
        msg = g_strconcat ("Couldn't save the change to the config file: ", &cfg_err->message, NULL);
        show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
        g_free (msg);
        g_clear_error (&cfg_err);
    }
    g_free (cfg_file_path);
    g_key_file_free (kf);
}