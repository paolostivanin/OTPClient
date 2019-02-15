#include <gtk/gtk.h>
#include <gcrypt.h>
#include <jansson.h>
#include "password-cb.h"
#include "message-dialogs.h"
#include "gquarks.h"
#include "db-misc.h"
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

    gchar *password = prompt_for_password (app_data, NULL, NULL, TRUE);

    gchar *exported_file_path = NULL;
    if (g_strcmp0 (action_name, "export_andotp") == 0) {
        exported_file_path = g_build_filename (base_dir, "andotp_exports.json.aes", NULL);
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
    }/* else if (g_strcmp0 (action_name, "export_authy") == 0) {
        // TODO: check authy format
        exported_file_path = g_build_filename (base_dir, "", NULL);
        export_authy (exported_file_path);
    } else if (g_strcmp0 (action_name, "export_winauth") == 0) {
        // TODO: check winauth format
        exported_file_path = g_build_filename (base_dir, "", NULL);
        export_winauth (exported_file_path);
    }*/ else {
        show_message_dialog (app_data->main_window, "Invalid export action.", GTK_MESSAGE_ERROR);
        return;
    }

    g_free (exported_file_path);
}
