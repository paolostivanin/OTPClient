#include <gtk/gtk.h>
#include <jansson.h>
#include "password-cb.h"
#include "message-dialogs.h"
#include "exports.h"


static void show_ret_msg_dialog (GtkWidget  *mainwin, gchar *fpath, gchar *ret_msg);


void
export_data_cb (GSimpleAction *simple,
                GVariant      *parameter __attribute__((unused)),
                gpointer       user_data)
{
    const gchar *action_name = g_action_get_name (G_ACTION(simple));
    AppData *app_data = (AppData *)user_data;

    const gchar *base_dir = NULL;
#ifndef USE_FLATPAK_APP_FOLDER
    base_dir = g_get_home_dir ();
#else
    base_dir = g_get_user_data_dir ();
#endif

    gchar *password = NULL, *exported_file_path = NULL, *ret_msg = NULL;
    gboolean encrypted = (g_strcmp0 (action_name, "export_andotp") == 0) ? TRUE : FALSE;
    if (g_strcmp0 (action_name, ANDOTP_EXPORT_ACTION_NAME) == 0 || g_strcmp0 (action_name, ANDOTP_EXPORT_PLAIN_ACTION_NAME) == 0) {
        if (encrypted == TRUE) {
            password = prompt_for_password (app_data, NULL, NULL, TRUE);
        }
        exported_file_path = g_build_filename (base_dir, encrypted == TRUE ? "andotp_exports.json.aes" : "andotp_exports.json", NULL);
        ret_msg = export_andotp (exported_file_path, password, app_data->db_data->json_data);
        show_ret_msg_dialog (app_data->main_window, exported_file_path, ret_msg);
    } else if (g_strcmp0 (action_name, FREEOTPPLUS_EXPORT_ACTION_NAME) == 0) {
        exported_file_path = g_build_filename (base_dir, "freeotpplus-exports.txt", NULL);
        ret_msg = export_freeotpplus (exported_file_path, app_data->db_data->json_data);
        show_ret_msg_dialog (app_data->main_window, exported_file_path, ret_msg);
    } else {
        show_message_dialog (app_data->main_window, "Invalid export action.", GTK_MESSAGE_ERROR);
        return;
    }
    g_free (ret_msg);
    g_free (exported_file_path);
}


static void
show_ret_msg_dialog (GtkWidget  *mainwin,
                     gchar      *fpath,
                     gchar      *ret_msg)
{
    GtkMessageType msg_type;
    gchar *message = NULL;

    if (ret_msg != NULL) {
        message = g_strconcat ("Error while exporting data: ", ret_msg, NULL);
        msg_type = GTK_MESSAGE_ERROR;
    } else {
        message = g_strconcat ("Data successfully exported to ", fpath, NULL);
        msg_type = GTK_MESSAGE_INFO;
    }
    show_message_dialog (mainwin, message, msg_type);
    g_free (message);
}
