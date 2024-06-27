#include <gtk/gtk.h>
#include <gcrypt.h>
#include <libsecret/secret.h>
#include "data.h"
#include "message-dialogs.h"
#include "password-cb.h"
#include "../common/secret-schema.h"
#include "otpclient.h"
#include "../common/macros.h"


void
change_password_cb (GSimpleAction *simple UNUSED,
                    GVariant      *parameter UNUSED,
                    gpointer       user_data)
{
    CAST_USER_DATA(AppData, app_data, user_data);
    gchar *tmp_key = secure_strdup (app_data->db_data->key);
    gchar *pwd = prompt_for_password (app_data, tmp_key, NULL, FALSE);
    if (pwd != NULL) {
        app_data->db_data->key = pwd;
        GError *err = NULL;
        update_db (app_data->db_data, &err);
        if (err != NULL) {
            show_message_dialog (app_data->main_window, err->message, GTK_MESSAGE_ERROR);
            GtkApplication *app = gtk_window_get_application (GTK_WINDOW(app_data->main_window));
            destroy_cb (app_data->main_window, app_data);
            g_application_quit (G_APPLICATION(app));
            return;
        }
        reload_db (app_data->db_data, &err);
        if (err != NULL) {
            show_message_dialog (app_data->main_window, err->message, GTK_MESSAGE_ERROR);
            GtkApplication *app = gtk_window_get_application (GTK_WINDOW(app_data->main_window));
            destroy_cb (app_data->main_window, app_data);
            g_application_quit (G_APPLICATION(app));
            return;
        }
        show_message_dialog (app_data->main_window, "Password successfully changed", GTK_MESSAGE_INFO);
        secret_password_store (OTPCLIENT_SCHEMA, SECRET_COLLECTION_DEFAULT, "main_pwd", app_data->db_data->key, NULL, on_password_stored, NULL, "string", "main_pwd", NULL);
    } else {
        gcry_free (tmp_key);
    }
}


void
change_pwd_cb_shortcut (GtkWidget *w UNUSED,
                        gpointer   user_data)
{
    change_password_cb (NULL, NULL, user_data);
}