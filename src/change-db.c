#include <gtk/gtk.h>
#include <gcrypt.h>
#include "data.h"
#include "message-dialogs.h"
#include "db-misc.h"
#include "password-cb.h"

static void select_file_icon_pressed_cb (GtkEntry         *entry,
                                         gint              position,
                                         GdkEventButton   *event,
                                         gpointer          data);

static void update_cfg_file             (AppData          *app_data);


void
change_db_cb (GSimpleAction *simple    __attribute__((unused)),
              GVariant      *parameter __attribute__((unused)),
              gpointer       user_data)
{
    AppData *app_data = (AppData *)user_data;

    GtkWidget *changedb_diag = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "changedb_diag_id"));
    GtkWidget *old_db_entry = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "changedb_olddb_entry_id"));
    GtkWidget *new_db_entry = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "changedb_entry_id"));
    g_signal_connect (new_db_entry, "icon-press", G_CALLBACK (select_file_icon_pressed_cb), app_data);

    gtk_entry_set_text (GTK_ENTRY(old_db_entry), app_data->db_data->db_path);

    const gchar *new_db_path;
    gint result = gtk_dialog_run (GTK_DIALOG (changedb_diag));
    switch (result) {
        case GTK_RESPONSE_OK:
            new_db_path = gtk_entry_get_text (GTK_ENTRY(new_db_entry));
            if (!g_file_test (new_db_path, G_FILE_TEST_IS_REGULAR) || g_file_test (new_db_path,G_FILE_TEST_IS_SYMLINK)){
                show_message_dialog (app_data->main_window, "Selected file is either a symlink or a non regular file.\nPlease choose another file.", GTK_MESSAGE_ERROR);
            } else {
                g_free (app_data->db_data->db_path);
                app_data->db_data->db_path = g_strdup (new_db_path);
                update_cfg_file (app_data);
                gcry_free (app_data->db_data->key);
                app_data->db_data->key = prompt_for_password (app_data, NULL, NULL, FALSE);
                GError *err = NULL;
                load_new_db (app_data, &err);
                if (err != NULL) {
                    show_message_dialog (app_data->main_window, err->message, GTK_MESSAGE_ERROR);
                    g_clear_error (&err);
                }
            }
            break;
        case GTK_RESPONSE_CANCEL:
        default:
            break;
    }
    gtk_widget_destroy (changedb_diag);
}


static void
select_file_icon_pressed_cb (GtkEntry         *entry,
                             gint              position __attribute__((unused)),
                             GdkEventButton   *event    __attribute__((unused)),
                             gpointer          data)
{
    AppData *app_data = (AppData *)data;

#if GTK_CHECK_VERSION(3, 20, 0)
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new ("Select database",
                                                                GTK_WINDOW(app_data->main_window),
                                                                GTK_FILE_CHOOSER_ACTION_OPEN,
                                                                "Open",
                                                                "Cancel");

    gint res = gtk_native_dialog_run (GTK_NATIVE_DIALOG(dialog));
#else
    GtkWidget *dialog = gtk_file_chooser_dialog_new ("Select database",
                                                     GTK_WINDOW(app_data->main_window),
                                                     GTK_FILE_CHOOSER_ACTION_OPEN,
                                                     "Cancel", GTK_RESPONSE_CANCEL,
                                                     "Open", GTK_RESPONSE_ACCEPT,
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


static void
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