#include <gtk/gtk.h>
#include <gcrypt.h>
#include <libsecret/secret.h>
#include "data.h"
#include "gui-misc.h"
#include "message-dialogs.h"
#include "password-cb.h"
#include "db-actions.h"
#include "../common/secret-schema.h"
#include "change-file-cb.h"
#include "../common/macros.h"

int
new_db (AppData *app_data)
{
    GtkWidget *newdb_diag = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "newdb_diag_id"));
    GtkWidget *newdb_entry = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "newdb_entry_id"));

    g_object_set_data (G_OBJECT(newdb_entry), "action", GINT_TO_POINTER(ACTION_SAVE));
    g_signal_connect (newdb_entry, "icon-press", G_CALLBACK (select_file_icon_pressed_cb), app_data);

    GString *new_db_path_with_suffix;
    gint result = gtk_dialog_run (GTK_DIALOG (newdb_diag));
    switch (result) {
        case GTK_RESPONSE_OK:
            if (gtk_entry_get_text_length (GTK_ENTRY(newdb_entry)) == 0) {
                show_message_dialog (app_data->main_window, "Input cannot be empty.", GTK_MESSAGE_ERROR);
                gtk_widget_hide (newdb_diag);
                return RETRY_CHANGE;
            }
            new_db_path_with_suffix = g_string_new (gtk_entry_get_text (GTK_ENTRY(newdb_entry)));
            g_string_append (new_db_path_with_suffix, ".enc");
            if (g_file_test (new_db_path_with_suffix->str, G_FILE_TEST_IS_REGULAR) || g_file_test (new_db_path_with_suffix->str, G_FILE_TEST_IS_SYMLINK)) {
                show_message_dialog (app_data->main_window, "Selected file already exists, please choose another filename.", GTK_MESSAGE_ERROR);
                g_string_free (new_db_path_with_suffix, TRUE);
                return RETRY_CHANGE;
            } else {
                gchar *old_db_path = g_strdup (app_data->db_data->db_path);
                g_free (app_data->db_data->db_path);
                app_data->db_data->db_path = g_strdup (new_db_path_with_suffix->str);
                update_cfg_file (app_data);
                gcry_free (app_data->db_data->key);
                app_data->db_data->key = prompt_for_password (app_data, NULL, NULL, FALSE);
                if (app_data->db_data->key == NULL) {
                    gtk_widget_hide (newdb_diag);
                    revert_db_path (app_data, old_db_path);
                    g_string_free (new_db_path_with_suffix, TRUE);
                    return RETRY_CHANGE;
                }
                secret_password_store (OTPCLIENT_SCHEMA, SECRET_COLLECTION_DEFAULT, "main_pwd", app_data->db_data->key, NULL, on_password_stored, NULL, "string", "main_pwd", NULL);
                GError *err = NULL;
                update_db (app_data->db_data, &err);
                if (err != NULL) {
                    show_message_dialog (app_data->main_window, err->message, GTK_MESSAGE_ERROR);
                    g_clear_error (&err);
                    gtk_widget_hide (newdb_diag);
                    revert_db_path (app_data, old_db_path);
                    g_string_free (new_db_path_with_suffix, TRUE);
                    return RETRY_CHANGE;
                }
                load_new_db (app_data, &err);
                if (err != NULL) {
                    show_message_dialog (app_data->main_window, err->message, GTK_MESSAGE_ERROR);
                    g_clear_error (&err);
                    gtk_widget_hide (newdb_diag);
                    revert_db_path (app_data, old_db_path);
                    g_string_free (new_db_path_with_suffix, TRUE);
                    return RETRY_CHANGE;
                }
                g_free (old_db_path);
            }
            g_string_free (new_db_path_with_suffix, TRUE);
            break;
        case GTK_RESPONSE_CANCEL:
        default:
            gtk_widget_hide (newdb_diag);
            return QUIT_APP;
    }
    gtk_widget_destroy (newdb_diag);

    return CHANGE_OK;
}


void
new_db_cb (GSimpleAction *simple UNUSED,
           GVariant      *parameter UNUSED,
           gpointer       user_data)
{
    CAST_USER_DATA(AppData, app_data, user_data);

    new_db (app_data);
}