#include <gtk/gtk.h>
#include <gcrypt.h>
#include <glib/gstdio.h>
#include <baseencode.h>
#include "imports.h"
#include "qrcode-parser.h"
#include "message-dialogs.h"
#include "add-common.h"
#include "get-builder.h"
#include "google-migration.pb-c.h"

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

static void     uri_received_func        (GtkClipboard  *clipboard,
                                          gchar        **uris,
                                          gpointer       user_data);

static void     image_received_func      (GtkClipboard  *clipboard,
                                          GdkPixbuf     *pixbuf,
                                          gpointer       user_data);

static GSList  *decode_migration_data    (const gchar   *uri);


void
add_qr_from_file (GSimpleAction *simple,
                  GVariant      *parameter __attribute__((unused)),
                  gpointer       user_data)
{
    const gchar *action_name = g_action_get_name (G_ACTION(simple));
    gboolean google_migration;
    (g_strcmp0 (action_name, GOOGLE_MIGRATION_ACTION_NAME) == 0) ? google_migration = TRUE : FALSE;

    AppData *app_data = (AppData *)user_data;

    GtkFileChooserNative *dialog = gtk_file_chooser_native_new ("Open File",
                                                     GTK_WINDOW (app_data->main_window),
                                                     GTK_FILE_CHOOSER_ACTION_OPEN,
                                                     "Open",
                                                     "Cancel");

    GtkFileFilter *filter = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter, "QR Image (*.png)");
    gtk_file_filter_add_pattern (filter, "*.png");
    gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (dialog), filter);

    gint res = gtk_native_dialog_run (GTK_NATIVE_DIALOG (dialog));

    if (res == GTK_RESPONSE_ACCEPT) {
        gchar *filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (dialog));
        parse_file_and_update_db (filename, app_data, google_migration);
        g_free (filename);
    }

    g_object_unref (dialog);
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

    GSList *otpauth_decoded_uris = NULL;
    if (google_migration == TRUE) {
        gint failed = 0;
        otpauth_decoded_uris = decode_migration_data (otpauth_uri);
        for (gint i = 0; i < g_slist_length (otpauth_decoded_uris); i++) {
            gchar *uri = g_slist_nth_data (otpauth_decoded_uris, i);
            err_msg = add_data_to_db (uri, app_data);
            if (err_msg != NULL) {
                failed++;
                g_free (err_msg);
            }
        }
        if (failed > 0) {
            GString *e_msg = g_string_new (NULL);
            g_string_printf (e_msg, "Failed to add all OTPs. Only %u out of %u were successfully added.", g_slist_length (otpauth_decoded_uris) - failed,
                             g_slist_length (otpauth_decoded_uris));
            show_message_dialog (app_data->main_window, e_msg->str, GTK_MESSAGE_ERROR);
            g_string_free (e_msg, TRUE);
        }
        g_slist_free_full (otpauth_decoded_uris, g_free);
    } else {
        err_msg = add_data_to_db (otpauth_uri, app_data);
        if (err_msg != NULL) {
            show_message_dialog (app_data->main_window, err_msg, GTK_MESSAGE_ERROR);
            g_free (err_msg);
        } else {
            show_message_dialog (app_data->main_window, "QRCode successfully imported.", GTK_MESSAGE_INFO);
        }
    }

    gcry_free (otpauth_uri);
}


static void
uri_received_func (GtkClipboard  *clipboard __attribute__((unused)),
                   gchar        **uris,
                   gpointer       user_data)
{
    AppData *app_data = (AppData *)user_data;
    GdkPixbuf *pbuf;
    GError *err = NULL;
    if (uris != NULL && uris[0] != NULL) {
        glong len_fpath = g_utf8_strlen (uris[0], -1) - 7 + 1; // -7 is for file://
        gchar *file_path = g_malloc0 (len_fpath);
        memcpy (file_path, uris[0] + 7, len_fpath);
        pbuf = gdk_pixbuf_new_from_file (file_path, &err);
        g_free (file_path);
        if (err != NULL) {
            gchar *msg = g_strconcat ("Couldn't get QR code URI from clipboard: ", err->message, NULL);
            show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
            g_free (msg);
        } else {
            // here we convert the input file to a PNG file, so we are able to parse it later on.
            gchar *filename = g_build_filename (g_get_tmp_dir (), "qrcode_from_cb_uri.png", NULL);
            gdk_pixbuf_save (pbuf, filename, "png", &err, NULL);
            parse_file_and_update_db (filename, app_data, FALSE);
            g_unlink (filename);
            g_free (filename);
            g_object_unref (pbuf);
        }
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
            parse_file_and_update_db (filename, app_data, FALSE);
        }
        g_unlink (filename);
        g_free (filename);
    } else {
        show_message_dialog (app_data->main_window, "Couldn't get QR code image from clipboard", GTK_MESSAGE_ERROR);
    }
}


static GSList *
decode_migration_data (const gchar *encoded_uri)
{
    const gchar *encoded_uri_copy = encoded_uri;
    if (g_ascii_strncasecmp (encoded_uri_copy, "otpauth-migration://offline?data=", 33) != 0) {
        return NULL;
    }
    encoded_uri_copy += 33;
    gsize out_len;
    guchar *data = g_base64_decode (g_uri_unescape_string ((encoded_uri_copy), NULL), &out_len);

    GSList *uris = NULL;
    gchar *uri = NULL;
    MigrationPayload *msg = migration_payload__unpack (NULL, out_len, data);
    for (gint i = 0; i < msg->n_otp_parameters; i++) {
        uri = g_strconcat ("otpauth://", NULL);
        if (msg->otp_parameters[i]->type == 1) {
            uri = g_strconcat (uri, "hotp/", NULL);
        } else if (msg->otp_parameters[i]->type == 2) {
            uri = g_strconcat (uri, "totp/", NULL);
        } else {
            g_printerr ("OTP type not recognized, skipping %s\n", msg->otp_parameters[i]->name);
            goto end;
        }

        uri = g_strconcat (uri, msg->otp_parameters[i]->name, "?", NULL);

        if (msg->otp_parameters[i]->algorithm == 1) {
            uri = g_strconcat (uri, "algorithm=SHA1&", NULL);
        } else if (msg->otp_parameters[i]->algorithm == 2) {
            uri = g_strconcat (uri, "algorithm=SHA256&", NULL);
        } else if (msg->otp_parameters[i]->algorithm == 3) {
            uri = g_strconcat (uri, "algorithm=SHA512&", NULL);
        } else {
            g_printerr ("Algorithm type not supported, skipping %s\n", msg->otp_parameters[i]->name);
            goto end;
        }

        if (msg->otp_parameters[i]->digits == 1) {
            uri = g_strconcat (uri, "digits=6&", NULL);
        } else if (msg->otp_parameters[i]->digits == 2) {
            uri = g_strconcat (uri, "digits=8&", NULL);
        } else {
            g_printerr ("Algorithm type not supported, skipping %s\n", msg->otp_parameters[i]->name);
            goto end;
        }

        if (msg->otp_parameters[i]->issuer != NULL) {
            uri = g_strconcat (uri, "issuer=", msg->otp_parameters[i]->issuer, "&", NULL);
        }

        if (msg->otp_parameters[i]->type == 1) {
            uri = g_strconcat (uri, "counter=", msg->otp_parameters[i]->counter, "&", NULL);
        }

        baseencode_error_t b_err;
        gchar *b32_encoded_secret = base32_encode (msg->otp_parameters[i]->secret.data, msg->otp_parameters[i]->secret.len, &b_err);
        if (b32_encoded_secret == NULL) {
            g_printerr ("Error while encoding the secret (error code %d)\n", b_err);
            goto end;
        }

        uri = g_strconcat (uri, "secret=", b32_encoded_secret, NULL);

        uris = g_slist_append (uris, g_strdup (uri));

        end:
        g_free (uri);
    }

    return uris;
}