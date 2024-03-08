#include <gtk/gtk.h>
#include <jansson.h>
#include <gcrypt.h>
#include "password-cb.h"
#include "message-dialogs.h"
#include "../common/exports.h"

static void     show_ret_msg_dialog       (GtkWidget   *mainwin,
                                           const gchar *fpath,
                                           const gchar *ret_msg);


void
export_data_cb (GSimpleAction *simple,
                GVariant      *parameter __attribute__((unused)),
                gpointer       user_data)
{
    const gchar *action_name = g_action_get_name (G_ACTION(simple));
    AppData *app_data = (AppData *)user_data;

    const gchar *base_dir = NULL;
#ifndef IS_FLATPAK
    base_dir = g_get_home_dir ();
#else
    base_dir = g_get_user_data_dir ();
#endif

    gboolean encrypted = FALSE;
    gchar *password = NULL;
    if (g_strcmp0 (action_name, "export_andotp") == 0 || g_strcmp0 (action_name, "export_aegis") == 0 ||
        g_strcmp0 (action_name, "export_authpro_enc") == 0 || g_strcmp0 (action_name, "export_twofas_enc") == 0) {
        password = prompt_for_password (app_data, NULL, NULL, TRUE);
        if (password == NULL) {
            return;
        }
        encrypted = TRUE;
    } else {
        const gchar *msg = "Please note that exporting to a plain format is a huge security risk.\n"
                            "If you wish to safely abort the operation, please click the 'Cancel' button below.";
        if (get_confirmation_from_dialog (app_data->main_window, msg) == FALSE) {
            return;
        }
    }

    GtkFileChooserNative *fl_diag = gtk_file_chooser_native_new ("Export file",
                                                                 GTK_WINDOW(app_data->main_window),
                                                                 GTK_FILE_CHOOSER_ACTION_SAVE,
                                                                 "OK",
                                                                 "Cancel");
    gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER(fl_diag), base_dir);
    gtk_file_chooser_set_do_overwrite_confirmation (GTK_FILE_CHOOSER(fl_diag), TRUE);
    gtk_file_chooser_set_select_multiple (GTK_FILE_CHOOSER(fl_diag), FALSE);

    const gchar *filename = NULL;
    if (g_strcmp0 (action_name, ANDOTP_EXPORT_ACTION_NAME) == 0 || g_strcmp0 (action_name, ANDOTP_EXPORT_PLAIN_ACTION_NAME) == 0) {
        filename = (encrypted == TRUE) ? "andotp_exports.json.aes" : "andotp_exports.json";
    } else if (g_strcmp0 (action_name, FREEOTPPLUS_EXPORT_ACTION_NAME) == 0) {
        filename = "freeotpplus-exports.txt";
    } else if (g_strcmp0 (action_name, AEGIS_EXPORT_ACTION_NAME) == 0 || g_strcmp0 (action_name, AEGIS_EXPORT_PLAIN_ACTION_NAME) == 0) {
        filename = (encrypted == TRUE) ? "aegis_encrypted.json" : "aegis_export_plain.json";
    } else if (g_strcmp0 (action_name, AUTHPRO_EXPORT_ENC_ACTION_NAME) == 0 || g_strcmp0 (action_name, AUTHPRO_EXPORT_PLAIN_ACTION_NAME) == 0) {
        filename = (encrypted == TRUE) ? "authpro_encrypted.bin" : "authpro_plain.json";
    } else if (g_strcmp0 (action_name, TWOFAS_EXPORT_ENC_ACTION_NAME) == 0 || g_strcmp0 (action_name, TWOFAS_EXPORT_PLAIN_ACTION_NAME) == 0) {
        filename = (encrypted == TRUE) ? "twofas_encrypted_v4.2fas" : "twofas_plain_v4.2fas";
    } else {
        show_message_dialog (app_data->main_window, "Invalid export action.", GTK_MESSAGE_ERROR);
        return;
    }

    gtk_file_chooser_set_current_name (GTK_FILE_CHOOSER(fl_diag), filename);

    gchar *export_file_abs_path = NULL;
    gint native_diag_res = gtk_native_dialog_run (GTK_NATIVE_DIALOG(fl_diag));
    if (native_diag_res == GTK_RESPONSE_ACCEPT) {
        export_file_abs_path = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER(fl_diag));
    }
    g_object_unref (fl_diag);

    if (export_file_abs_path == NULL) {
        show_message_dialog (app_data->main_window, "Invalid export file name/path.", GTK_MESSAGE_ERROR);
        if (encrypted == TRUE) {
            gcry_free (password);
        }
        return;
    }

    gchar *ret_msg = NULL;
    if (g_strcmp0 (action_name, ANDOTP_EXPORT_ACTION_NAME) == 0 || g_strcmp0 (action_name, ANDOTP_EXPORT_PLAIN_ACTION_NAME) == 0) {
        ret_msg = export_andotp (export_file_abs_path, password, app_data->db_data->json_data);
    } else if (g_strcmp0 (action_name, FREEOTPPLUS_EXPORT_ACTION_NAME) == 0) {
        ret_msg = export_freeotpplus (export_file_abs_path, app_data->db_data->json_data);
    } else if (g_strcmp0 (action_name, AEGIS_EXPORT_ACTION_NAME) == 0 || g_strcmp0 (action_name, AEGIS_EXPORT_PLAIN_ACTION_NAME) == 0) {
        ret_msg = export_aegis (export_file_abs_path, password, app_data->db_data->json_data);
    } else if (g_strcmp0 (action_name, AUTHPRO_EXPORT_ENC_ACTION_NAME) == 0 || g_strcmp0 (action_name, AUTHPRO_EXPORT_PLAIN_ACTION_NAME) == 0) {
        ret_msg = export_authpro (export_file_abs_path, password, app_data->db_data->json_data);
    } else if (g_strcmp0 (action_name, TWOFAS_EXPORT_ENC_ACTION_NAME) == 0 || g_strcmp0 (action_name, TWOFAS_EXPORT_PLAIN_ACTION_NAME) == 0) {
        ret_msg = export_twofas (export_file_abs_path, password, app_data->db_data->json_data);
    } else {
        show_message_dialog (app_data->main_window, "Invalid export action.", GTK_MESSAGE_ERROR);
        return;
    }
    show_ret_msg_dialog (app_data->main_window, export_file_abs_path, ret_msg);
    g_free (ret_msg);
    g_free (export_file_abs_path);
    if (encrypted == TRUE) {
        gcry_free (password);
    }
}


static void
show_ret_msg_dialog (GtkWidget   *mainwin,
                     const gchar *fpath,
                     const gchar *ret_msg)
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
