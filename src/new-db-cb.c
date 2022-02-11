#include <gtk/gtk.h>
#include <gcrypt.h>
#include "data.h"
#include "db-misc.h"
#include "message-dialogs.h"
#include "password-cb.h"
#include "db-actions.h"

void
new_db_cb (GSimpleAction *simple    __attribute__((unused)),
           GVariant      *parameter __attribute__((unused)),
           gpointer       user_data)
{
    AppData *app_data = (AppData *)user_data;
    GtkWidget *newdb_diag = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "newdb_diag_id"));
    GtkWidget *newdb_entry = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "newdb_entry_id"));

    g_object_set_data (G_OBJECT(newdb_entry), "action", GINT_TO_POINTER(ACTION_SAVE));
    g_signal_connect (newdb_entry, "icon-press", G_CALLBACK (select_file_icon_pressed_cb), app_data);

    const gchar *new_db_path;
    gint result = gtk_dialog_run (GTK_DIALOG (newdb_diag));
    switch (result) {
        case GTK_RESPONSE_OK:
            new_db_path = gtk_entry_get_text (GTK_ENTRY(newdb_entry));
            if (g_file_test (new_db_path, G_FILE_TEST_IS_REGULAR) || g_file_test (new_db_path,G_FILE_TEST_IS_SYMLINK)){
                show_message_dialog (app_data->main_window, "Selected file already exists, please choose another filename.", GTK_MESSAGE_ERROR);
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
    gtk_widget_destroy (newdb_diag);
}