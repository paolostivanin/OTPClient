#include <gtk/gtk.h>
#include <gcrypt.h>
#include <jansson.h>
#include "password-cb.h"
#include "message-dialogs.h"
#include "../common/import-export.h"
#include "gui-misc.h"
#include "../common/macros.h"


static gboolean  parse_data_and_update_db    (AppData       *app_data,
                                              const gchar   *filename,
                                              const gchar   *action_name);


void
import_data_cb (GSimpleAction *simple,
                GVariant      *parameter UNUSED,
                gpointer       user_data)
{
    const gchar *action_name = g_action_get_name (G_ACTION(simple));
    CAST_USER_DATA(AppData, app_data, user_data);

    GtkFileChooserNative *dialog = gtk_file_chooser_native_new ("Open File",
                                                     GTK_WINDOW(app_data->main_window),
                                                     GTK_FILE_CHOOSER_ACTION_OPEN,
                                                     "Open",
                                                     "Cancel");

    gint res = gtk_native_dialog_run (GTK_NATIVE_DIALOG(dialog));

    if (res == GTK_RESPONSE_ACCEPT) {
        GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
        gchar *filename = gtk_file_chooser_get_filename (chooser);
        parse_data_and_update_db (app_data, filename, action_name);
        g_free (filename);
    }

    g_object_unref (dialog);
}


static gboolean
parse_data_and_update_db (AppData       *app_data,
                          const gchar   *filename,
                          const gchar   *action_name)
{
    GError *err = NULL;
    gchar *pwd = NULL;

    if (g_strcmp0 (action_name, ANDOTP_ENC_ACTION_NAME) == 0 || g_strcmp0 (action_name, AEGIS_ENC_ACTION_NAME) == 0 ||
        g_strcmp0 (action_name, AUTHPRO_ENC_ACTION_NAME) == 0 || g_strcmp0 (action_name, TWOFAS_ENC_ACTION_NAME) == 0) {
        pwd = prompt_for_password (app_data, NULL, action_name, FALSE);
        if (pwd == NULL) {
            return FALSE;
        }
    }

    GSList *content = get_data_from_provider (action_name, filename, pwd, app_data->db_data->max_file_size_from_memlock, &err);
    if (content == NULL) {
        const gchar *msg = "An error occurred while importing, so nothing has been added to the database.";
        gchar *msg_with_err = NULL;
        if (err != NULL) {
            msg_with_err = g_strconcat (msg, " The error is:\n", err->message, NULL);
        }
        show_message_dialog (app_data->main_window, err == NULL ? msg : msg_with_err, GTK_MESSAGE_ERROR);
        g_free (msg_with_err);
        if (err != NULL){
            g_clear_error (&err);
        }
        if (pwd != NULL) {
            gcry_free (pwd);
        }
        return FALSE;
    }

    gchar *err_msg = update_db_from_otps (content, app_data);
    if (err_msg != NULL) {
        show_message_dialog (app_data->main_window, err_msg, GTK_MESSAGE_ERROR);
        g_free (err_msg);
        if (pwd != NULL) {
            gcry_free (pwd);
        }
        return FALSE;
    }

    if (pwd != NULL) {
        gcry_free (pwd);
    }
    free_otps_gslist (content, g_slist_length (content));

    return TRUE;
}
