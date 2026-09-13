#include <glib/gi18n.h>
#include <gio/gio.h>
#include <zbar.h>
#include "webcam-scanner.h"
#include "gquarks.h"


static void webcam_scan_thread (GTask        *task,
                                gpointer      source_object,
                                gpointer      task_data,
                                GCancellable *cancellable);

static gchar *webcam_scan_qrcode_cancellable (GCancellable *cancellable,
                                               GError      **err);

gchar *
webcam_scan_qrcode (GError **err)
{
    return webcam_scan_qrcode_cancellable (NULL, err);
}

static gchar *
webcam_scan_qrcode_cancellable (GCancellable *cancellable,
                                GError      **err)
{
    zbar_processor_t *proc = zbar_processor_create (1);
    if (proc == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "%s", _("Failed to create zbar processor"));
        return NULL;
    }

    /* "" rather than NULL, and no display. Both arguments matter and both were
     * wrong, which is why webcam scanning had never once worked in this build:
     *
     * zbar_processor_init only creates a video source inside `if (dev)`, so
     * with dev == NULL there was none, and the `if (!dev && !enable_display)
     * goto done` shortcut did not fire either because the display was enabled.
     * Init therefore returned 0, and every call after it failed on zbar's
     * `if (!proc->video)` guard with "video input not initialized". An empty
     * string is what zbarcam itself passes: zbar_video_open maps any dev whose
     * first byte is below 0x10 to /dev/video0, so this asks for the default
     * camera rather than hardcoding a path.
     *
     * enable_display = 1 asked for a zbar preview window, and zbar/window has
     * backends for X11 and Windows only, no Wayland. Under Flatpak's
     * fallback-x11 on a Wayland session there is no X display to open at all.
     * The scan itself does not need a window; a GTK-rendered preview is a
     * separate feature. */
    if (zbar_processor_init (proc, "", 0) != 0) {
        g_warning ("zbar could not open the camera: %s",
                   zbar_processor_error_string (proc, 1));
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "%s", _("Failed to initialize zbar video device"));
        zbar_processor_destroy (proc);
        return NULL;
    }

    /* No zbar window to make visible any more, and the return value is worth
     * having: this is the call that used to fail silently. */
    if (zbar_processor_set_active (proc, 1) != 0) {
        g_warning ("zbar could not start the capture: %s",
                   zbar_processor_error_string (proc, 1));
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "%s", _("Failed to initialize zbar video device"));
        zbar_processor_destroy (proc);
        return NULL;
    }

    /* Poll in short intervals so closing/locking the window releases the
     * camera promptly instead of waiting for one 30-second blocking call. */
    int rc = 0;
    for (guint i = 0; i < 120 && rc == 0; i++) {
        if (cancellable != NULL &&
            g_cancellable_is_cancelled (cancellable)) {
            zbar_processor_destroy (proc);
            g_set_error (err, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                         "%s", _("Webcam scan cancelled"));
            return NULL;
        }
        rc = zbar_process_one (proc, 250);
    }
    if (rc <= 0) {
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

    /* zbar_processor_get_results hands back a counted reference, unlike
     * zbar_image_get_symbols, which only borrows one. Without this the symbol
     * set outlives the processor and leaks on every successful scan. */
    zbar_symbol_set_ref (symbols, -1);
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
    if (g_cancellable_is_cancelled (cancellable)) {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                 "%s", _("Webcam scan cancelled"));
        return;
    }

    GError *err = NULL;
    gchar *uri = webcam_scan_qrcode_cancellable (cancellable, &err);
    if (uri == NULL) {
        g_task_return_error (task, err);
        return;
    }
    if (g_cancellable_is_cancelled (cancellable)) {
        g_free (uri);
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                 "%s", _("Webcam scan cancelled"));
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
