#include <gtk/gtk.h>
#include <jansson.h>
#include "password-cb.h"
#include "message-dialogs.h"
#include "exports.h"


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

    gchar *password = NULL;
    gboolean encrypted = (g_strcmp0 (action_name, "export_andotp") == 0) ? TRUE : FALSE;
    gchar *exported_file_path = NULL;
    if (g_strcmp0 (action_name, "export_andotp") == 0 || g_strcmp0 (action_name, "export_andotp_plain") == 0) {
        if (encrypted == TRUE) {
            password = prompt_for_password (app_data, NULL, NULL, TRUE);
        }
        exported_file_path = g_build_filename (base_dir, encrypted == TRUE ? "andotp_exports.json.aes" : "andotp_exports.json", NULL);
        gchar *message = NULL;
        GtkMessageType msg_type;
        gchar *ret_msg = export_andotp (exported_file_path, password, app_data->db_data->json_data);
        if (ret_msg != NULL) {
            message = g_strconcat ("Error while exporting data: ", ret_msg, NULL);
            msg_type = GTK_MESSAGE_ERROR;
        } else {
            message = g_strconcat ("Data successfully exported to ", exported_file_path, NULL);
            msg_type = GTK_MESSAGE_INFO;
        }
        show_message_dialog (app_data->main_window, message, msg_type);
        g_free (message);
        g_free (ret_msg);
    } else {
        show_message_dialog (app_data->main_window, "Invalid export action.", GTK_MESSAGE_ERROR);
        return;
    }

    g_free (exported_file_path);
}
