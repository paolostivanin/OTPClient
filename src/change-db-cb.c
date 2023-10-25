#include <gtk/gtk.h>
#include <gcrypt.h>
#include <libsecret/secret.h>
#include "data.h"
#include "message-dialogs.h"
#include "db-misc.h"
#include "password-cb.h"
#include "db-actions.h"
#include "secret-schema.h"


void
change_db_cb (GSimpleAction *simple    __attribute__((unused)),
              GVariant      *parameter __attribute__((unused)),
              gpointer       user_data)
{
    AppData *app_data = (AppData *)user_data;

    GtkWidget *changedb_diag = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "changedb_diag_id"));
    GtkWidget *old_changedb_entry = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "changedb_olddb_entry_id"));
    GtkWidget *new_changedb_entry = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "changedb_entry_id"));

    g_object_set_data (G_OBJECT(new_changedb_entry), "action", GINT_TO_POINTER(ACTION_OPEN));
    g_signal_connect (new_changedb_entry, "icon-press", G_CALLBACK (select_file_icon_pressed_cb), app_data);

    gtk_entry_set_text (GTK_ENTRY(old_changedb_entry), app_data->db_data->db_path);

    const gchar *new_db_path;
    gint result = gtk_dialog_run (GTK_DIALOG (changedb_diag));
    switch (result) {
        case GTK_RESPONSE_OK:
            new_db_path = gtk_entry_get_text (GTK_ENTRY(new_changedb_entry));
            if (!g_file_test (new_db_path, G_FILE_TEST_IS_REGULAR) || g_file_test (new_db_path,G_FILE_TEST_IS_SYMLINK)){
                show_message_dialog (app_data->main_window, "Selected file is either a symlink or a non regular file.\nPlease choose another file.", GTK_MESSAGE_ERROR);
            } else {
                g_free (app_data->db_data->db_path);
                app_data->db_data->db_path = g_strdup (new_db_path);
                update_cfg_file (app_data);
                gcry_free (app_data->db_data->key);
                app_data->db_data->key = prompt_for_password (app_data, NULL, NULL, FALSE);
                secret_password_store (OTPCLIENT_SCHEMA, SECRET_COLLECTION_DEFAULT, "main_pwd", app_data->db_data->key, NULL, on_password_stored, NULL, "string", "main_pwd", NULL);
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


void
change_db_cb_shortcut (GtkWidget *w __attribute__((unused)),
                       gpointer   user_data)
{
    change_db_cb (NULL, NULL, user_data);
}
