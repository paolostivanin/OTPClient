#include <glib/gi18n.h>
#include <gio/gio.h>
#include <zbar.h>
#include "webcam-scanner.h"
#include "gquarks.h"


static void webcam_scan_thread (GTask        *task,
                                gpointer      source_object,
                                gpointer      task_data,
                                GCancellable *cancellable);


gchar *
webcam_scan_qrcode (GError **err)
{
    zbar_processor_t *proc = zbar_processor_create (1);
    if (proc == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "%s", _("Failed to create zbar processor"));
        return NULL;
    }

    if (zbar_processor_init (proc, NULL, 1) != 0) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "%s", _("Failed to initialize zbar video device"));
        zbar_processor_destroy (proc);
        return NULL;
    }

    zbar_processor_set_visible (proc, 1);
    zbar_processor_set_active (proc, 1);

    /* Wait for a scan result with a 30-second timeout */
    int rc = zbar_process_one (proc, 30000);
    if (rc < 0) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "%s", _("Webcam scanning failed or timed out"));
        zbar_processor_destroy (proc);
        return NULL;
    }

    const zbar_symbol_set_t *symbols = zbar_processor_get_results (proc);
    if (symbols == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "%s", _("No QR code detected"));
        zbar_processor_destroy (proc);
        return NULL;
    }

    const zbar_symbol_t *symbol = zbar_symbol_set_first_symbol (symbols);
    gchar *result = NULL;
    while (symbol != NULL) {
        zbar_symbol_type_t type = zbar_symbol_get_type (symbol);
        if (type == ZBAR_QRCODE) {
            const char *data = zbar_symbol_get_data (symbol);
            if (data != NULL) {
                result = g_strdup (data);
                break;
            }
        }
        symbol = zbar_symbol_next (symbol);
    }

    zbar_processor_destroy (proc);

    if (result == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "%s", _("No QR code found in webcam scan"));
    }

    return result;
}


static void
webcam_scan_thread (GTask        *task,
                    gpointer      source_object G_GNUC_UNUSED,
                    gpointer      task_data     G_GNUC_UNUSED,
                    GCancellable *cancellable   G_GNUC_UNUSED)
{
    GError *err = NULL;
    gchar *uri = webcam_scan_qrcode (&err);
    if (uri == NULL) {
        g_task_return_error (task, err);
        return;
    }
    g_task_return_pointer (task, uri, g_free);
}


void
webcam_scan_qrcode_async (GCancellable        *cancellable,
                          GAsyncReadyCallback  callback,
                          gpointer             user_data)
{
    GTask *task = g_task_new (NULL, cancellable, callback, user_data);
    g_task_run_in_thread (task, webcam_scan_thread);
    g_object_unref (task);
}


gchar *
webcam_scan_qrcode_finish (GAsyncResult  *result,
                           GError       **err)
{
    g_return_val_if_fail (g_task_is_valid (result, NULL), NULL);
    return g_task_propagate_pointer (G_TASK (result), err);
}
