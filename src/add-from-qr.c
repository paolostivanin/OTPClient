#include <gtk/gtk.h>
#include <gcrypt.h>
#include "imports.h"
#include "qrcode-parser.h"
#include "message-dialogs.h"
#include "add-common.h"

static void parse_file_and_update_db (const gchar   *filename,
                                      AppData       *app_data);

static void uri_received_func        (GtkClipboard  *clipboard,
                                      gchar        **uris,
                                      gpointer       user_data);


void
add_qr_from_file (GSimpleAction *simple    __attribute__((unused)),
                  GVariant      *parameter __attribute__((unused)),
                  gpointer       user_data)
{
    AppData *app_data = (AppData *)user_data;

#if GTK_CHECK_VERSION(3, 20, 0)
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new ("Open File",
                                                     GTK_WINDOW (app_data->main_window),
                                                     GTK_FILE_CHOOSER_ACTION_OPEN,
                                                     "Open",
                                                     "Cancel");
#else
    GtkWidget *dialog = gtk_file_chooser_dialog_new ("Open File",
                                                     GTK_WINDOW (app_data->main_window),
                                                     GTK_FILE_CHOOSER_ACTION_OPEN,
                                                     "Cancel", GTK_RESPONSE_CANCEL,
                                                     "Open", GTK_RESPONSE_ACCEPT,
                                                     NULL);
#endif
    GtkFileFilter *filter = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter, "QR Image (*.png)");
    gtk_file_filter_add_pattern (filter, "*.png");
    gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (dialog), filter);

#if GTK_CHECK_VERSION(3, 20, 0)
    gint res = gtk_native_dialog_run (GTK_NATIVE_DIALOG (dialog));
#else
    gint res = gtk_dialog_run (GTK_DIALOG (dialog));
#endif
    if (res == GTK_RESPONSE_ACCEPT) {
        gchar *filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (dialog));
        parse_file_and_update_db (filename, app_data);
        g_free (filename);
    }
#if GTK_CHECK_VERSION(3, 20, 0)
    g_object_unref (dialog);
#else
    gtk_widget_destroy (dialog);
#endif
}


void
add_qr_from_clipboard (GSimpleAction *simple    __attribute__((unused)),
                       GVariant      *parameter __attribute__((unused)),
                       gpointer       user_data)
{
    AppData *app_data = (AppData *)user_data;
    gint timeout = 0;
    gboolean uris_available = FALSE;
    while (uris_available == FALSE || timeout < 30) {
        uris_available = gtk_clipboard_wait_is_uris_available (app_data->clipboard);
        timeout++;
        g_usleep (1 * G_USEC_PER_SEC);
    }

    if (uris_available == TRUE) {
        gtk_clipboard_request_uris (app_data->clipboard, (GtkClipboardURIReceivedFunc)uri_received_func, app_data);
    } else {
        show_message_dialog (app_data->main_window, "Operation timed out after 30 seconds.\nNo QR code could be found in the clipboard.", GTK_MESSAGE_ERROR);
    }
}


static void
parse_file_and_update_db (const gchar *filename,
                          AppData     *app_data)
{
    gchar *otpauth_uri = NULL;
    gchar *err_msg = parse_qrcode (filename, &otpauth_uri);
    if (err_msg != NULL) {
        show_message_dialog(app_data->main_window, err_msg, GTK_MESSAGE_ERROR);
        g_free(err_msg);
        return;
    }

    err_msg = add_data_to_db (otpauth_uri, app_data);
    if (err_msg != NULL) {
        show_message_dialog (app_data->main_window, err_msg, GTK_MESSAGE_ERROR);
        g_free (err_msg);
    } else {
        show_message_dialog (app_data->main_window, "QRCode successfully imported from the screenshot", GTK_MESSAGE_INFO);
    }
    gcry_free (otpauth_uri);
}


static void
uri_received_func (GtkClipboard  *clipboard __attribute__((unused)),
                   gchar        **uris,
                   gpointer       user_data)
{
    AppData *app_data = (AppData *)user_data;
    if (uris != NULL && uris[0] != NULL) {
        gint len_fpath = g_utf8_strlen (uris[0], -1) - 7 + 1; // -7 is for file://
        gchar *file_path = g_malloc0 (len_fpath);
        memcpy (file_path + 7, uris[0], len_fpath);
        parse_file_and_update_db (file_path, app_data);
        g_free (file_path);
    } else {
        show_message_dialog (app_data->main_window, "Couldn't get QR code URI from clipboard", GTK_MESSAGE_ERROR);
    }
}