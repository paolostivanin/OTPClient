#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include "db-misc.h"
#include "message-dialogs.h"

gboolean
change_file (AppData *app_data)
{
    gboolean res = FALSE;
    GtkWidget *label = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "diag_changefile_label_id"));
    gchar *partial_msg_start = g_markup_printf_escaped ("%s <span font_family=\"monospace\">%s</span>", "The currently selected file is:\n", app_data->db_data->db_path);
    const gchar *partial_msg_end = "\n\nDo you want to change it?\n\n"
                               "If you select <b>Yes</b>, you will be asked to pick another\n"
                               "database and then you will be prompted for the\n"
                               "decryption password.\n\n"
                               "\nIf you select <b>No</b>, then the app will close.";
    gchar *msg = g_strconcat (partial_msg_start, partial_msg_end, NULL);
    gtk_label_set_markup (GTK_LABEL(label), msg);
    g_free (msg);
    g_free (partial_msg_start);

    GtkFileChooserNative *fl_diag;
    GtkWidget *diag_changefile = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "diag_changefile_id"));
    gint result = gtk_dialog_run (GTK_DIALOG(diag_changefile));
    switch (result) {
        case GTK_RESPONSE_OK:
            fl_diag = gtk_file_chooser_native_new ("Open File",
                                            GTK_WINDOW(app_data->main_window),
                                            GTK_FILE_CHOOSER_ACTION_OPEN,
                                            "Open",
                                            "Cancel");

            gint native_diag_res = gtk_native_dialog_run (GTK_NATIVE_DIALOG(fl_diag));
            if (native_diag_res == GTK_RESPONSE_ACCEPT) {
                GtkFileChooser *chooser = GTK_FILE_CHOOSER(fl_diag);
                gchar *db_path = gtk_file_chooser_get_filename (chooser);
                GKeyFile *kf = g_key_file_new ();
                gchar *cfg_file_path;
#ifndef USE_FLATPAK_APP_FOLDER
                cfg_file_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
#else
                cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#endif
                g_key_file_set_string (kf, "config", "db_path", db_path);
                GError *err = NULL;
                if (!g_key_file_save_to_file (kf, cfg_file_path, &err)) {
                    gchar *err_msg = g_strconcat (_("Couldn't save the config file: "), err->message, NULL);
                    show_message_dialog (app_data->main_window, err_msg, GTK_MESSAGE_ERROR);
                    g_free (err_msg);
                    g_clear_error (&err);
                }
                g_free (app_data->db_data->db_path);
                app_data->db_data->db_path = g_strdup (db_path);
                g_free (db_path);
                g_free (cfg_file_path);
                g_key_file_free (kf);
            }
            g_object_unref (fl_diag);
            res = TRUE;
            break;
        case GTK_RESPONSE_CANCEL:
        default:
            break;
    }
    gtk_widget_hide (diag_changefile);

    return res;
}