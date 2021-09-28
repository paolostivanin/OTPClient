#pragma once

#include <glib.h>

G_BEGIN_DECLS

gchar *parse_qrcode (const gchar   *png_path,
                     gchar        **otpauth_uri);

G_END_DECLS
