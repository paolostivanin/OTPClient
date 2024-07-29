#include <glib.h>
#include <zbar.h>
#include <png.h>
#include <gcrypt.h>
#include "../common/common.h"
#include "gui-misc.h"


gchar *
parse_qrcode (const gchar    *png_path,
              gchar         **otpauth_uri)
{
    zbar_image_scanner_t *scanner = zbar_image_scanner_create ();
    zbar_image_scanner_set_config (scanner, ZBAR_NONE, ZBAR_CFG_ENABLE, 1);

    GdkPixbuf *pbuf = gdk_pixbuf_new_from_file (png_path, NULL);
    gint width = gdk_pixbuf_get_width (pbuf);
    gint height = gdk_pixbuf_get_height (pbuf);
    guchar *raw_data = gdk_pixbuf_get_pixels (pbuf);
    gint rowstride = gdk_pixbuf_get_rowstride (pbuf);
    gint n_channels = gdk_pixbuf_get_n_channels (pbuf);

    guchar *gray_data = g_malloc0 (width * height);

    // we need to convert RGB data to grayscale, otherwise QR parsing will fail
    for (gint y = 0; y < height; y++) {
        for (gint x = 0; x < width; x++) {
            guchar *p = raw_data + y * rowstride + x * n_channels;
            gray_data[y * width + x] = (p[0] * 0.299) + (p[1] * 0.587) + (p[2] * 0.114);
        }
    }
    g_object_unref (pbuf);

    zbar_image_t *image = zbar_image_create ();
    zbar_image_set_format (image, zbar_fourcc ('Y','8','0','0'));
    zbar_image_set_size (image, width, height);
    zbar_image_set_data (image, gray_data, width * height, zbar_image_free_data);

    gint n = zbar_scan_image (scanner, image);
    if (n < 1) {
        zbar_image_destroy (image);
        zbar_image_scanner_destroy (scanner);
        return g_strdup ("Couldn't find a valid qrcode");
    }

    const zbar_symbol_t *symbol = zbar_image_first_symbol (image);
    for (; symbol; symbol = zbar_symbol_next (symbol)) {
        gchar *unesc_str = g_uri_unescape_string_secure (zbar_symbol_get_data (symbol), NULL);
        *otpauth_uri = secure_strdup (unesc_str);
        gcry_free (unesc_str);
    }

    zbar_image_destroy (image);
    zbar_image_scanner_destroy (scanner);

    return NULL;
}
