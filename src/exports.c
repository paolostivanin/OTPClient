#include <gtk/gtk.h>
#include <jansson.h>
#include <gcrypt.h>
#include <glib/gi18n.h>
#include "password-cb.h"
#include "message-dialogs.h"
#include "common/exports.h"


enum export_format_t {
    ANDOTP,
    FREEOTPPLUS,
    AEGIS
};


static void show_ret_msg_dialog (GtkWidget  *mainwin, gchar *fpath, gchar *ret_msg);

static void
on_save_response                      (GtkDialog *dialog,
                                       int        response,
                                       gpointer   user_data,
                                       gboolean   encrypted,
                                       enum export_format_t format);

static void
on_save_response_andotp_encrypted     (GtkDialog *dialog,
                                       int        response,
                                       gpointer   user_data);
static void
on_save_response_andotp_plaintext     (GtkDialog *dialog,
                                       int        response,
                                       gpointer   user_data);
static void
on_save_response_freeotpplus_plaintext(GtkDialog *dialog,
                                       int        response,
                                       gpointer   user_data);
static void
on_save_response_aegis_encrypted      (GtkDialog *dialog,
                                       int        response,
                                       gpointer   user_data);
static void
on_save_response_aegis_plaintext      (GtkDialog *dialog,
                                       int        response,
                                       gpointer   user_data);


int export_is_encrypted(const gchar *action_name)
{
    if ((g_strcmp0 (action_name, "export_andotp") == 0) || (g_strcmp0 (action_name, "export_aegis") == 0)) {
        return TRUE;
    } else {
        return FALSE;
    }
}

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

    gboolean encrypted = export_is_encrypted(action_name);

    // ...
    GtkWidget *dialog;
    GtkFileChooser *chooser;
    GtkFileChooserAction action = GTK_FILE_CHOOSER_ACTION_SAVE;

    dialog = gtk_file_chooser_dialog_new (_("Export File"),
                                          GTK_WINDOW (app_data->main_window), // FIXME; no idea if you are allowed to make this cast
                                          action,
                                          _("_Cancel"),
                                          GTK_RESPONSE_CANCEL,
                                          _("_Save"),
                                          GTK_RESPONSE_ACCEPT,
                                          NULL);
    chooser = GTK_FILE_CHOOSER (dialog);
    gtk_file_chooser_set_current_folder (chooser, base_dir);
    gtk_file_chooser_set_do_overwrite_confirmation (chooser, TRUE);

    if (g_strcmp0 (action_name, ANDOTP_EXPORT_ACTION_NAME) == 0 || g_strcmp0 (action_name, ANDOTP_EXPORT_PLAIN_ACTION_NAME) == 0) {
        gtk_file_chooser_set_current_name (chooser, encrypted == TRUE ? "andotp_exports.json.aes" : "andotp_exports.json");
        gtk_window_present (GTK_WINDOW (dialog));
        if (encrypted == TRUE) {
            g_signal_connect (dialog, "response",
                              G_CALLBACK (on_save_response_andotp_encrypted),
                              user_data);
        } else {
            g_signal_connect (dialog, "response",
                              G_CALLBACK (on_save_response_andotp_plaintext),
                              user_data);
          }
    } else if (g_strcmp0 (action_name, FREEOTPPLUS_EXPORT_ACTION_NAME) == 0) {
        gtk_file_chooser_set_current_name (chooser, "freeotpplus-exports.txt");
        gtk_window_present (GTK_WINDOW (dialog));
        g_signal_connect (dialog, "response",
                          G_CALLBACK (on_save_response_freeotpplus_plaintext),
                          user_data);
    } else if (g_strcmp0 (action_name, AEGIS_EXPORT_ACTION_NAME) == 0 || g_strcmp0 (action_name, AEGIS_EXPORT_PLAIN_ACTION_NAME) == 0) {
        gtk_file_chooser_set_current_name (chooser, encrypted == TRUE ? "aegis_encrypted.json" : "aegis_export_plain.json");
        gtk_window_present (GTK_WINDOW (dialog));
        if (encrypted == TRUE) {
            g_signal_connect (dialog, "response",
                              G_CALLBACK (on_save_response_aegis_encrypted),
                              user_data);
        } else {
            g_signal_connect (dialog, "response",
                              G_CALLBACK (on_save_response_aegis_plaintext),
                              user_data);
        }
    } else {
        show_message_dialog (app_data->main_window, _("Invalid export action."), GTK_MESSAGE_ERROR);
        return;
    }

}

static void
on_save_response_andotp_encrypted (GtkDialog *dialog,
                                   int        response,
                                   gpointer   user_data)
{
    enum export_format_t format = ANDOTP;
    on_save_response(dialog, response, user_data, TRUE, format);
}
static void
on_save_response_andotp_plaintext (GtkDialog *dialog,
                                   int        response,
                                   gpointer   user_data)
{
    enum export_format_t format = ANDOTP;
    on_save_response(dialog, response, user_data, FALSE, format);
}

static void
on_save_response_freeotpplus_plaintext (GtkDialog *dialog,
                                        int        response,
                                        gpointer   user_data)
{
    enum export_format_t format = FREEOTPPLUS;
    on_save_response(dialog, response, user_data, FALSE, format);
}

static void
on_save_response_aegis_encrypted (GtkDialog *dialog,
                                  int        response,
                                  gpointer   user_data)
{
    enum export_format_t format = AEGIS;
    on_save_response(dialog, response, user_data, TRUE, format);
}
static void
on_save_response_aegis_plaintext (GtkDialog *dialog,
                                  int        response,
                                  gpointer   user_data)
{
    enum export_format_t format = AEGIS;
    on_save_response(dialog, response, user_data, FALSE, format);
}

static void
on_save_response (GtkDialog *dialog,
                  int        response,
                  gpointer   user_data,
                  gboolean   encrypted,
                  enum export_format_t format)
{
    AppData *app_data = (AppData *)user_data;
    gchar *password = NULL, *exported_file_path = NULL, *ret_msg = NULL;

    if (response == GTK_RESPONSE_ACCEPT)
    {
        GtkFileChooser *chooser = GTK_FILE_CHOOSER (dialog);
        exported_file_path = gtk_file_chooser_get_filename (chooser);
        gtk_widget_destroy ( GTK_WIDGET(dialog));
        if (encrypted == TRUE) {
            password = prompt_for_password (app_data, NULL, NULL, TRUE);
            if (password == NULL) {
                goto null_password;
            }
        }
        switch (format)
        {
            case ANDOTP:
                ret_msg = export_andotp     (exported_file_path, password, app_data->db_data->json_data);
                break;
            case FREEOTPPLUS:
                ret_msg = export_freeotpplus(exported_file_path, app_data->db_data->json_data);
                break;
            case AEGIS:
                ret_msg = export_aegis      (exported_file_path, app_data->db_data->json_data, password);
                break;
            default: /*NOTREACHED*/
                break;
        }
        show_ret_msg_dialog (app_data->main_window, exported_file_path, ret_msg);
        if (encrypted == TRUE) {
            gcry_free (password);
        }
        g_free (ret_msg);
null_password:
        g_free (exported_file_path);
    }
    else if (response == GTK_RESPONSE_CANCEL)
    {
        gtk_widget_destroy ( GTK_WIDGET(dialog));
    }
}

static void
show_ret_msg_dialog (GtkWidget  *mainwin,
                     gchar      *fpath,
                     gchar      *ret_msg)
{
    GtkMessageType msg_type;
    gchar *message = NULL;

    if (ret_msg != NULL) {
        message = g_strconcat (_("Error while exporting data: "), ret_msg, NULL);
        msg_type = GTK_MESSAGE_ERROR;
    } else {
        message = g_strconcat (_("Data successfully exported to "), fpath, NULL);
        msg_type = GTK_MESSAGE_INFO;
    }
    show_message_dialog (mainwin, message, msg_type);
    g_free (message);
}
