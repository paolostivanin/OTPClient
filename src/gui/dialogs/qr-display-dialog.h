#pragma once

#include <adwaita.h>
#include <jansson.h>

G_BEGIN_DECLS

#define QR_DISPLAY_TYPE_DIALOG (qr_display_dialog_get_type ())

G_DECLARE_FINAL_TYPE (QrDisplayDialog, qr_display_dialog, QR_DISPLAY, DIALOG, AdwDialog)

QrDisplayDialog *qr_display_dialog_new (const gchar *otpauth_uri,
                                         const gchar *label);

G_END_DECLS
