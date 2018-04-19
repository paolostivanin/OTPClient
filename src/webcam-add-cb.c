#include <gtk/gtk.h>
#include <zbar.h>
#include <gcrypt.h>
#include "get-builder.h"
#include "common.h"
#include "imports.h"
#include "parse-uri.h"
#include "message-dialogs.h"


typedef struct _config_data {
    GtkWidget *diag;
    zbar_processor_t *proc;
    gchar *otp_uri;
    gboolean qrcode_found;
    gboolean gtimeout_exit_value;
    guint id;
    guint counter;
} ConfigData;

static void     scan_qrcode     (zbar_image_t   *image,
                                 gconstpointer   user_data);

static gboolean check_result    (gpointer        data);

static void     add_data_to_db  (const gchar    *otp_uri,
                                 ImportData     *import_data);


void
webcam_cb (GSimpleAction *simple    __attribute__((unused)),
           GVariant      *parameter __attribute__((unused)),
           gpointer       user_data)
{
    ImportData *import_data = (ImportData *)user_data;

    ConfigData *cfg_data = g_new0 (ConfigData, 1);

    GtkBuilder *builder = get_builder_from_partial_path ("share/otpclient/webcam-diag.ui");
    cfg_data->diag = GTK_WIDGET (gtk_builder_get_object (builder, "diag_webcam"));
    gtk_window_set_transient_for (GTK_WINDOW (cfg_data->diag), GTK_WINDOW (import_data->main_window));

    cfg_data->qrcode_found = FALSE;
    cfg_data->gtimeout_exit_value = TRUE;
    cfg_data->counter = 0;
    
    cfg_data->proc = zbar_processor_create (1);
    zbar_processor_set_config (cfg_data->proc, ZBAR_NONE, ZBAR_CFG_ENABLE, 1);
    if (zbar_processor_init (cfg_data->proc, "/dev/video0", 1)) {
        show_message_dialog (import_data->main_window, "Couldn't initialize the webcam", GTK_MESSAGE_ERROR);
        zbar_processor_destroy (cfg_data->proc);
        g_free (cfg_data);
        g_object_unref (builder);
        return;
    }
    zbar_processor_set_data_handler (cfg_data->proc, scan_qrcode, cfg_data);
    zbar_processor_set_visible (cfg_data->proc, 0);
    zbar_processor_set_active (cfg_data->proc, 1);

    cfg_data->id = g_timeout_add (1000, check_result, cfg_data);

    gtk_widget_show_all (cfg_data->diag);

    gint response = gtk_dialog_run (GTK_DIALOG (cfg_data->diag));
    if (response == GTK_RESPONSE_CANCEL) {
        if (cfg_data->qrcode_found) {
            add_data_to_db (cfg_data->otp_uri, import_data);
            gcry_free (cfg_data->otp_uri);
        }
        zbar_processor_destroy (cfg_data->proc);
        if (cfg_data->gtimeout_exit_value) {
            // only remove if 'check_result' returned TRUE
            g_source_remove (cfg_data->id);
        }
        gtk_widget_destroy (cfg_data->diag);
        g_free (cfg_data);
        g_object_unref (builder);
    }
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
        cfg_data->otp_uri = secure_strdup (zbar_symbol_get_data (symbol));
        cfg_data->qrcode_found = TRUE;
    }
}


static void
add_data_to_db (const gchar *otp_uri, ImportData *import_data)
{
    GtkListStore *list_store = g_object_get_data (G_OBJECT (import_data->main_window), "lstore");
    GSList *otps = NULL;
    set_otps_from_uris (otp_uri, &otps);
    if (g_slist_length (otps) < 1) {
        show_message_dialog (import_data->main_window, "No valid otpauth uris found", GTK_MESSAGE_ERROR);
        return;
    }
    gchar *err_msg = update_db_from_otps (otps, import_data->db_data, list_store);
    if (err_msg != NULL) {
        show_message_dialog (import_data->main_window, err_msg, GTK_MESSAGE_ERROR);
        g_free (err_msg);
    }
    free_otps_gslist (otps, g_slist_length (otps));
}