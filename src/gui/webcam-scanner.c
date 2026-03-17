#include <glib/gi18n.h>
#include <zbar.h>
#include "webcam-scanner.h"
#include "gquarks.h"


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
