#include <gtk/gtk.h>
#include <zbar.h>
#include <gcrypt.h>
#include "imports.h"
#include "parse-uri.h"
#include "message-dialogs.h"
#include "get-builder.h"
#include "common/common.h"
#include "gui-common.h"


typedef struct config_data_t {
    GtkWidget *diag;
    gchar *otp_uri;
    gboolean qrcode_found;
    gboolean gtimeout_exit_value;
    guint counter;
} ConfigData;

static gboolean check_result    (gpointer        data);

static void     scan_qrcode     (zbar_image_t   *image,
                                 gconstpointer   user_data);


void
webcam_add_cb (GSimpleAction *simple,
               GVariant      *parameter __attribute__((unused)),
               gpointer       user_data)
{
    const gchar *action_name = g_action_get_name (G_ACTION(simple));
    gboolean google_migration = (g_strcmp0 (action_name, GOOGLE_MIGRATION_WEBCAM_ACTION_NAME) == 0) ? TRUE : FALSE;

    AppData *app_data = (AppData *)user_data;

    ConfigData *cfg_data = g_new0 (ConfigData, 1);

    GtkBuilder *builder = get_builder_from_partial_path (UI_PARTIAL_PATH);
    cfg_data->diag = GTK_WIDGET(gtk_builder_get_object (builder, "diag_webcam_id"));

    cfg_data->qrcode_found = FALSE;
    cfg_data->gtimeout_exit_value = TRUE;
    cfg_data->counter = 0;

    zbar_processor_t *proc = zbar_processor_create (1);
    zbar_processor_set_config (proc, ZBAR_NONE, ZBAR_CFG_ENABLE, 1);
    if (zbar_processor_init (proc, "/dev/video0", 1)) {
        show_message_dialog (app_data->main_window, "Couldn't initialize the webcam", GTK_MESSAGE_ERROR);
        zbar_processor_destroy (proc);
        g_free (cfg_data);
        return;
    }
    zbar_processor_set_data_handler (proc, scan_qrcode, cfg_data);
    zbar_processor_set_visible (proc, 1);
    zbar_processor_set_active (proc, 1);

    guint source_id = g_timeout_add (1000, check_result, cfg_data);

    gtk_widget_show_all (cfg_data->diag);

    gint response = gtk_dialog_run (GTK_DIALOG (cfg_data->diag));
    if (response == GTK_RESPONSE_CANCEL) {
        if (cfg_data->qrcode_found) {
            zbar_processor_destroy (proc);
            gchar *err_msg = parse_uris_migration (app_data, cfg_data->otp_uri, google_migration);
            if (err_msg != NULL) {
                show_message_dialog (app_data->main_window, err_msg, GTK_MESSAGE_ERROR);
                g_free (err_msg);
            } else {
                show_message_dialog (app_data->main_window, "QRCode successfully scanned", GTK_MESSAGE_INFO);
            }
            gcry_free (cfg_data->otp_uri);
        }
        if (cfg_data->gtimeout_exit_value) {
            // only remove if 'check_result' returned TRUE
            g_source_remove (source_id);
        }
        gtk_widget_destroy (cfg_data->diag);
        g_free (cfg_data);
    }
    g_object_unref (builder);
}


void
webcam_add_cb_shortcut (GtkWidget *w __attribute__((unused)),
                        gpointer   user_data)
{
    webcam_add_cb (g_simple_action_new ("webcam", NULL), NULL, user_data);
}


static gboolean
check_result (gpointer data)
{
    ConfigData *cfg_data = (ConfigData *)data;
    if (cfg_data->qrcode_found || cfg_data->counter > 30) {
        gtk_dialog_response (GTK_DIALOG (cfg_data->diag), GTK_RESPONSE_CANCEL);
        cfg_data->gtimeout_exit_value = FALSE;
        return FALSE;
    }
    cfg_data->counter++;
    return TRUE;
}


static void
scan_qrcode (zbar_image_t   *image,
             gconstpointer   user_data)
{
    ConfigData *cfg_data = (ConfigData *)user_data;
    const zbar_symbol_t *symbol = zbar_image_first_symbol (image);
    for (; symbol; symbol = zbar_symbol_next (symbol)) {
        gchar *unesc_str = g_uri_unescape_string_secure (zbar_symbol_get_data (symbol), NULL);
        cfg_data->otp_uri = secure_strdup (unesc_str);
        cfg_data->qrcode_found = TRUE;
        gcry_free (unesc_str);
    }
}
