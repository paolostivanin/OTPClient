#pragma once

#include <glib.h>
#include <gdk/gdk.h>

G_BEGIN_DECLS

gchar *qrcode_parse_image_file (const gchar  *filepath,
                                 GError      **error);

gchar *qrcode_parse_texture    (GdkTexture   *texture,
                                 GError      **error);

G_END_DECLS
