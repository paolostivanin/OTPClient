#include <gtk/gtk.h>
#include <gcrypt.h>
#include <libsecret/secret.h>
#include "data.h"
#include "db-misc.h"
#include "message-dialogs.h"
#include "password-cb.h"
#include "db-actions.h"
#include "secret-schema.h"

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

    GString *new_db_path_with_suffix;
    gint result = gtk_dialog_run (GTK_DIALOG (newdb_diag));
    switch (result) {
        case GTK_RESPONSE_OK:
            new_db_path_with_suffix = g_string_new (gtk_entry_get_text (GTK_ENTRY(newdb_entry)));
            g_string_append (new_db_path_with_suffix, ".enc");
            if (g_file_test (new_db_path_with_suffix->str, G_FILE_TEST_IS_REGULAR) || g_file_test (new_db_path_with_suffix->str, G_FILE_TEST_IS_SYMLINK)) {
                show_message_dialog (app_data->main_window, "Selected file already exists, please choose another filename.", GTK_MESSAGE_ERROR);
            } else {
                g_free (app_data->db_data->db_path);
                app_data->db_data->db_path = g_strdup (new_db_path_with_suffix->str);
                update_cfg_file (app_data);
                gcry_free (app_data->db_data->key);
                app_data->db_data->key = prompt_for_password (app_data, NULL, NULL, FALSE);
                secret_password_store (OTPCLIENT_SCHEMA, SECRET_COLLECTION_DEFAULT, "main_pwd", app_data->db_data->key, NULL, on_password_stored, NULL, "string", "main_pwd", NULL);
                GError *err = NULL;
                write_db_to_disk (app_data->db_data, &err);
                if (err != NULL) {
                    show_message_dialog (app_data->main_window, err->message, GTK_MESSAGE_ERROR);
                    g_clear_error (&err);
                } else {
                    load_new_db (app_data, &err);
                    if (err != NULL) {
                        show_message_dialog (app_data->main_window, err->message, GTK_MESSAGE_ERROR);
                        g_clear_error (&err);
                    }
                }
            }
            g_string_free (new_db_path_with_suffix, TRUE);
            break;
        case GTK_RESPONSE_CANCEL:
        default:
            break;
    }
    gtk_widget_destroy (newdb_diag);
}