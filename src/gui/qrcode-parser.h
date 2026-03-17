#pragma once

#include <glib.h>

G_BEGIN_DECLS

gchar *qrcode_parse_image_file (const gchar  *filepath,
                                 GError      **error);

G_END_DECLS
