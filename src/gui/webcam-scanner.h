#pragma once

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

gchar *webcam_scan_qrcode (GError **err);

void   webcam_scan_qrcode_async  (GCancellable        *cancellable,
                                  GAsyncReadyCallback  callback,
                                  gpointer             user_data);

gchar *webcam_scan_qrcode_finish (GAsyncResult        *result,
                                  GError             **err);

G_END_DECLS
