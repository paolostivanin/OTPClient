#include "gtk-compat.h"
#include <gcrypt.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include "../common/import-export.h"
#include "../common/macros.h"
#include "qrcode-parser.h"
#include "message-dialogs.h"
#include "get-builder.h"
#include "gui-misc.h"


typedef struct gtimeout_data_t {
    GtkWidget *diag;
    gboolean uris_available;
    gboolean image_available;
    gboolean gtimeout_exit_value;
    guint counter;
    AppData * app_data;
} GTimeoutCBData;

static gboolean check_result             (gpointer       data);

static void     parse_file_and_update_db (const gchar   *filename,
                                          AppData       *app_data,
                                          gboolean       google_migration);

static void     uri_received_func        (GObject       *source_object,
                                          GAsyncResult  *result,
                                          gpointer       user_data);

static void     image_received_func      (GObject       *source_object,
                                          GAsyncResult  *result,
                                          gpointer       user_data);

static void     parse_clipboard_text     (const gchar   *text,
                                          AppData       *app_data);

static void     free_pixbuf_pixels       (guchar        *pixels,
                                          gpointer       user_data);


void
add_qr_from_file (GSimpleAction *simple,
                  GVariant      *parameter UNUSED,
                  gpointer       user_data)
{
    const gchar *action_name = g_action_get_name (G_ACTION(simple));
    gboolean google_migration = (g_strcmp0 (action_name, GOOGLE_FILE_ACTION_NAME) == 0) ? TRUE : FALSE;

    CAST_USER_DATA(AppData, app_data, user_data);

    GtkFileChooserNative *dialog = gtk_file_chooser_native_new ("Open File",
                                                     GTK_WINDOW (app_data->main_window),
                                                     GTK_FILE_CHOOSER_ACTION_OPEN,
                                                     "Open",
                                                     "Cancel");

    GtkFileFilter *filter = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter, "QR Image (*.png, *.jpeg)");
    gtk_file_filter_add_pattern (filter, "*.png");
    gtk_file_filter_add_pattern (filter, "*.jpeg");
    gtk_file_filter_add_pattern (filter, "*.jpg");
    gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (dialog), filter);

    gint res = gtk_native_dialog_run (GTK_NATIVE_DIALOG (dialog));

    if (res == GTK_RESPONSE_ACCEPT) {
        GFile *file = gtk_file_chooser_get_file (GTK_FILE_CHOOSER (dialog));
        if (file != NULL) {
            gchar *filename = g_file_get_path (file);
            if (filename != NULL) {
                parse_file_and_update_db (filename, app_data, google_migration);
                g_free (filename);
            }
            g_object_unref (file);
        }
    }

    g_object_unref (dialog);
}


void
add_qr_from_clipboard (GSimpleAction *simple UNUSED,
                       GVariant      *parameter UNUSED,
                       gpointer       user_data)
{
    CAST_USER_DATA(AppData, app_data, user_data);
    if (app_data->clipboard == NULL) {
        show_message_dialog (app_data->main_window, "Clipboard not available.", GTK_MESSAGE_ERROR);
        return;
    }
    GTimeoutCBData *gt_cb_data = g_new0 (GTimeoutCBData, 1);
    gt_cb_data->uris_available = FALSE;
    gt_cb_data->image_available = FALSE;
    gt_cb_data->gtimeout_exit_value = TRUE;
    gt_cb_data->counter = 0;
    gt_cb_data->app_data = app_data;

    guint source_id = g_timeout_add (1000, check_result, gt_cb_data);

    GtkBuilder *builder = get_builder_from_partial_path (UI_PARTIAL_PATH);
    gt_cb_data->diag = GTK_WIDGET(gtk_builder_get_object (builder, "diag_qr_clipboard_id"));
    gtk_window_present(GTK_WINDOW(gt_cb_data->diag));

    gint response = gtk_dialog_run (GTK_DIALOG (gt_cb_data->diag));
    if (response == GTK_RESPONSE_CANCEL) {
        if (gt_cb_data->uris_available == TRUE) {
            gdk_clipboard_read_text_async (app_data->clipboard, NULL, uri_received_func, app_data);
        }
        if (gt_cb_data->image_available == TRUE) {
            gdk_clipboard_read_texture_async (app_data->clipboard, NULL, image_received_func, app_data);
        }
        if (gt_cb_data->gtimeout_exit_value == TRUE) {
            // only remove if 'check_result' returned TRUE
            g_source_remove (source_id);
        }
        gtk_window_destroy (GTK_WINDOW(gt_cb_data->diag));
        g_free (gt_cb_data);
    }
    g_object_unref (builder);
}


static gboolean
check_result (gpointer data)
{
    GTimeoutCBData *gt_cb_data = (GTimeoutCBData *)data;
    GdkContentFormats *formats = NULL;
    if (gt_cb_data->app_data->clipboard != NULL) {
        formats = gdk_clipboard_get_formats(gt_cb_data->app_data->clipboard);
    }
    gt_cb_data->uris_available = formats != NULL
        && (gdk_content_formats_contain_mime_type(formats, "text/uri-list")
            || gdk_content_formats_contain_mime_type(formats, "text/plain"));
    gt_cb_data->image_available = formats != NULL
        && (gdk_content_formats_contain_mime_type(formats, "image/png")
            || gdk_content_formats_contain_mime_type(formats, "image/jpeg")
            || gdk_content_formats_contain_mime_type(formats, "image/bmp"));
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
                          AppData     *app_data,
                          gboolean     google_migration)
{
    gchar *otpauth_uri = NULL;
    gchar *err_msg = parse_qrcode (filename, &otpauth_uri);
    if (err_msg != NULL) {
        show_message_dialog(app_data->main_window, err_msg, GTK_MESSAGE_ERROR);
        g_free(err_msg);
        return;
    }

    err_msg = parse_uris_migration (app_data, otpauth_uri, google_migration);
    if (err_msg != NULL) {
        show_message_dialog (app_data->main_window, err_msg, GTK_MESSAGE_ERROR);
        g_free (err_msg);
    } else {
        show_message_dialog (app_data->main_window, "QRCode successfully scanned", GTK_MESSAGE_INFO);
    }

    gcry_free (otpauth_uri);
}


static void
uri_received_func (GObject      *source_object,
                   GAsyncResult *result,
                   gpointer      user_data)
{
    CAST_USER_DATA(AppData, app_data, user_data);
    GError *err = NULL;
    gchar *text = gdk_clipboard_read_text_finish(GDK_CLIPBOARD(source_object), result, &err);
    if (err != NULL) {
        gchar *msg = g_strconcat ("Couldn't get QR code URI from clipboard: ", err->message, NULL);
        show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
        g_free (msg);
        g_clear_error (&err);
    } else if (text != NULL) {
        parse_clipboard_text (text, app_data);
        g_free (text);
    } else {
        show_message_dialog (app_data->main_window, "Couldn't get QR code URI from clipboard", GTK_MESSAGE_ERROR);
    }
}


static void
image_received_func (GObject      *source_object,
                     GAsyncResult *result,
                     gpointer      user_data)
{
    CAST_USER_DATA(AppData, app_data, user_data);
    GError  *err = NULL;
    GdkTexture *texture = gdk_clipboard_read_texture_finish(GDK_CLIPBOARD(source_object), result, &err);
    if (texture != NULL) {
        int width = gdk_texture_get_width(texture);
        int height = gdk_texture_get_height(texture);
        gsize stride = (gsize)width * 4;
        guchar *pixels = g_malloc(stride * height);
        gdk_texture_download(texture, pixels, stride);
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_data(pixels, GDK_COLORSPACE_RGB, TRUE, 8, width, height, stride, free_pixbuf_pixels, NULL);
        gchar *filename = g_build_filename (g_get_tmp_dir (), "qrcode_from_cb.png", NULL);
        gdk_pixbuf_save (pixbuf, filename, "png", &err, NULL);
        if (err != NULL) {
            gchar *msg = g_strconcat ("Couldn't save clipboard to png:\n", err->message, NULL);
            show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
            g_free (msg);
            g_clear_error (&err);
        } else {
            parse_file_and_update_db (filename, app_data, FALSE);
        }
        if (g_unlink (filename) == -1) {
            g_printerr ("%s\n", _("Error while unlinking the temp png."));
        }
        g_free (filename);
        g_object_unref (pixbuf);
        g_object_unref (texture);
    } else {
        if (err != NULL) {
            gchar *msg = g_strconcat ("Couldn't get QR code image from clipboard: ", err->message, NULL);
            show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
            g_free (msg);
            g_clear_error (&err);
            return;
        }
        show_message_dialog (app_data->main_window, "Couldn't get QR code image from clipboard", GTK_MESSAGE_ERROR);
    }
}

static void
parse_clipboard_text (const gchar *text,
                      AppData     *app_data)
{
    gchar **lines = g_strsplit (text, "\n", -1);
    gchar *first_line = NULL;
    for (gint i = 0; lines[i] != NULL; i++) {
        if (lines[i][0] != '\0') {
            first_line = lines[i];
            break;
        }
    }

    if (first_line == NULL) {
        show_message_dialog (app_data->main_window, "Couldn't get QR code URI from clipboard", GTK_MESSAGE_ERROR);
        g_strfreev (lines);
        return;
    }

    if (g_str_has_prefix (first_line, "file://")) {
        GError *err = NULL;
        gchar *filename = g_filename_from_uri (first_line, NULL, &err);
        if (filename == NULL) {
            gchar *msg = g_strconcat ("Couldn't get QR code URI from clipboard: ", err->message, NULL);
            show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
            g_free (msg);
            g_clear_error (&err);
            g_strfreev (lines);
            return;
        }
        parse_file_and_update_db (filename, app_data, FALSE);
        g_free (filename);
    } else if (g_str_has_prefix (first_line, "otpauth://") || g_str_has_prefix (first_line, "otpauth-migration://")) {
        gchar *err_msg = parse_uris_migration (app_data, first_line, FALSE);
        if (err_msg != NULL) {
            show_message_dialog (app_data->main_window, err_msg, GTK_MESSAGE_ERROR);
            g_free (err_msg);
        } else {
            show_message_dialog (app_data->main_window, "QRCode successfully scanned", GTK_MESSAGE_INFO);
        }
    } else {
        show_message_dialog (app_data->main_window, "Couldn't get QR code URI from clipboard", GTK_MESSAGE_ERROR);
    }
    g_strfreev (lines);
}

static void
free_pixbuf_pixels (guchar   *pixels,
                    gpointer  user_data UNUSED)
{
    g_free (pixels);
}
