#include <gtk/gtk.h>
#include <gcrypt.h>
#include <glib/gstdio.h>
#include "imports.h"
#include "qrcode-parser.h"
#include "message-dialogs.h"
#include "add-common.h"
#include "get-builder.h"

typedef struct _gtimeout_data {
    GtkWidget *diag;
    gboolean uris_available;
    gboolean image_available;
    gboolean gtimeout_exit_value;
    guint counter;
    AppData * app_data;
} GTimeoutCBData;

static gboolean check_result             (gpointer data);

static void     parse_file_and_update_db (const gchar   *filename,
                                          AppData       *app_data);

static void     uri_received_func        (GtkClipboard  *clipboard,
                                          gchar        **uris,
                                          gpointer       user_data);

static void     image_received_func (GtkClipboard       *clipboard,
                                     GdkPixbuf          *pixbuf,
                                     gpointer            user_data);


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
    GTimeoutCBData *gt_cb_data = g_new0 (GTimeoutCBData, 1);
    gt_cb_data->uris_available = FALSE;
    gt_cb_data->image_available = FALSE;
    gt_cb_data->gtimeout_exit_value = TRUE;
    gt_cb_data->counter = 0;
    gt_cb_data->app_data = app_data;

    guint source_id = g_timeout_add (1000, check_result, gt_cb_data);

    GtkBuilder *builder = get_builder_from_partial_path (UI_PARTIAL_PATH);
    gt_cb_data->diag = GTK_WIDGET(gtk_builder_get_object (builder, "diag_qr_clipboard_id"));
    gtk_widget_show_all (gt_cb_data->diag);

    gint response = gtk_dialog_run (GTK_DIALOG (gt_cb_data->diag));
    if (response == GTK_RESPONSE_CANCEL) {
        if (gt_cb_data->uris_available == TRUE) {
            gtk_clipboard_request_uris (app_data->clipboard, (GtkClipboardURIReceivedFunc)uri_received_func, app_data);
        }
        if (gt_cb_data->image_available == TRUE) {
            gtk_clipboard_request_image (app_data->clipboard, (GtkClipboardImageReceivedFunc)image_received_func, app_data);
        }
        if (gt_cb_data->gtimeout_exit_value == TRUE) {
            // only remove if 'check_result' returned TRUE
            g_source_remove (source_id);
        }
        gtk_widget_destroy (gt_cb_data->diag);
        g_free (gt_cb_data);
    }
    g_object_unref (builder);
}


static gboolean
check_result (gpointer data)
{
    GTimeoutCBData *gt_cb_data = (GTimeoutCBData *)data;
    gt_cb_data->uris_available = gtk_clipboard_wait_is_uris_available (gt_cb_data->app_data->clipboard);
    gt_cb_data->image_available = gtk_clipboard_wait_is_image_available (gt_cb_data->app_data->clipboard);
    if (gt_cb_data->counter > 30 || gt_cb_data->uris_available == TRUE || gt_cb_data->image_available == TRUE) {
        gtk_dialog_response (GTK_DIALOG (gt_cb_data->diag), GTK_RESPONSE_CANCEL);
        gt_cb_data->gtimeout_exit_value = FALSE;
        return FALSE;
    }
    gt_cb_data->counter++;
    return TRUE;
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
        memcpy (file_path, uris[0] + 7, len_fpath);
        parse_file_and_update_db (file_path, app_data);
        g_free (file_path);
    } else {
        show_message_dialog (app_data->main_window, "Couldn't get QR code URI from clipboard", GTK_MESSAGE_ERROR);
    }
}


static void
image_received_func (GtkClipboard  *clipboard __attribute__((unused)),
                     GdkPixbuf     *pixbuf,
                     gpointer       user_data)
{
    AppData *app_data = (AppData *)user_data;
    GError  *err = NULL;
    if (pixbuf != NULL) {
        gchar *filename = g_build_filename (g_get_tmp_dir (), "qrcode_from_cb.png", NULL);
        gdk_pixbuf_save (pixbuf, filename, "png", &err, NULL);
        if (err != NULL) {
            gchar *msg = g_strconcat ("Couldn't save clipboard to png:\n", err->message, NULL);
            show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
            g_free (msg);
        } else {
            parse_file_and_update_db (filename, app_data);
        }
        g_unlink (filename);
        g_free (filename);
    } else {
        show_message_dialog (app_data->main_window, "Couldn't get QR code image from clipboard", GTK_MESSAGE_ERROR);
    }
}