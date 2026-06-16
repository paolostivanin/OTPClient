#include <glib/gi18n.h>
#include <zbar.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include "qrcode-parser.h"
#include "gquarks.h"

/* Real-world QR code images for OTP enrollment are at most a few hundred
 * pixels per side. Cap the dimensions before any allocation: a malicious
 * source (image file, oversized clipboard texture) with width=height=65535
 * would otherwise request huge decode buffers. g_malloc aborts on huge
 * allocations, which crashes the application — DoS via a single QR import. */
#define MAX_QR_IMAGE_DIM 4096u

static gboolean
checked_mul_size (gsize   a,
                  gsize   b,
                  gsize  *out)
{
    if (a != 0 && b > G_MAXSIZE / a)
        return FALSE;
    *out = a * b;
    return TRUE;
}

static gchar *
scan_grayscale_buffer (const guchar  *gray,
                       guint          width,
                       guint          height,
                       GError       **error)
{
    zbar_image_scanner_t *scanner = zbar_image_scanner_create ();
    zbar_image_scanner_set_config (scanner, 0, ZBAR_CFG_ENABLE, 0);
    zbar_image_scanner_set_config (scanner, ZBAR_QRCODE, ZBAR_CFG_ENABLE, 1);

    zbar_image_t *image = zbar_image_create ();
    zbar_image_set_format (image, zbar_fourcc ('Y', '8', '0', '0'));
    zbar_image_set_size (image, width, height);
    zbar_image_set_data (image, gray, width * height, NULL);

    gint n = zbar_scan_image (scanner, image);
    gchar *result = NULL;

    if (n > 0)
    {
        for (const zbar_symbol_t *symbol = zbar_image_first_symbol (image);
             symbol != NULL;
             symbol = zbar_symbol_next (symbol))
        {
            if (zbar_symbol_get_type (symbol) != ZBAR_QRCODE)
                continue;
            const gchar *data = zbar_symbol_get_data (symbol);
            if (data != NULL) {
                result = g_strdup (data);
                break;
            }
        }
        if (result == NULL)
            g_set_error (error, generic_error_gquark (), GENERIC_ERRCODE,
                         "No QR code found in the image");
    }
    else
    {
        g_set_error (error, generic_error_gquark (), GENERIC_ERRCODE,
                     "No QR code found in the image");
    }

    zbar_image_destroy (image);
    zbar_image_scanner_destroy (scanner);

    return result;
}

static gboolean
load_pixbuf_image (const gchar  *filepath,
                   guchar      **raw_data,
                   guint        *width,
                   guint        *height,
                   GError      **error)
{
    gint info_width = 0;
    gint info_height = 0;
    if (gdk_pixbuf_get_file_info (filepath, &info_width, &info_height) == NULL) {
        g_set_error (error, generic_error_gquark (), GENERIC_ERRCODE,
                     "Unsupported or corrupt image file");
        return FALSE;
    }
    if (info_width <= 0 || info_height <= 0 ||
        (guint) info_width > MAX_QR_IMAGE_DIM ||
        (guint) info_height > MAX_QR_IMAGE_DIM) {
        g_set_error (error, generic_error_gquark (), GENERIC_ERRCODE,
                     "Image dimensions out of range (%dx%d, max %ux%u).",
                     info_width, info_height, MAX_QR_IMAGE_DIM, MAX_QR_IMAGE_DIM);
        return FALSE;
    }

    g_autoptr (GdkPixbuf) pixbuf = gdk_pixbuf_new_from_file (filepath, error);
    if (pixbuf == NULL)
        return FALSE;

    int w = gdk_pixbuf_get_width (pixbuf);
    int h = gdk_pixbuf_get_height (pixbuf);
    int channels = gdk_pixbuf_get_n_channels (pixbuf);
    int rowstride = gdk_pixbuf_get_rowstride (pixbuf);
    if (w <= 0 || h <= 0 || channels < 3 ||
        (guint) w > MAX_QR_IMAGE_DIM || (guint) h > MAX_QR_IMAGE_DIM) {
        g_set_error (error, generic_error_gquark (), GENERIC_ERRCODE,
                     "Decoded image layout is invalid.");
        return FALSE;
    }

    gsize gray_size = 0;
    if (!checked_mul_size ((gsize) w, (gsize) h, &gray_size)) {
        g_set_error (error, generic_error_gquark (), GENERIC_ERRCODE,
                     "Image allocation size overflow.");
        return FALSE;
    }

    const guchar *pixels = gdk_pixbuf_read_pixels (pixbuf);
    guchar *gray = g_malloc (gray_size);
    for (int y = 0; y < h; y++) {
        const guchar *row = pixels + ((gsize) y * (gsize) rowstride);
        for (int x = 0; x < w; x++) {
            const guchar *px = row + ((gsize) x * (gsize) channels);
            gray[(gsize) y * (gsize) w + (gsize) x] =
                (guchar)(0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2]);
        }
    }

    *raw_data = gray;
    *width = (guint) w;
    *height = (guint) h;
    return TRUE;
}

gchar *
qrcode_parse_image_file (const gchar  *filepath,
                          GError      **error)
{
    guchar *raw_data = NULL;
    guint width = 0, height = 0;

    if (!load_pixbuf_image (filepath, &raw_data, &width, &height, error))
        return NULL;

    gchar *result = scan_grayscale_buffer (raw_data, width, height, error);
    g_free (raw_data);

    return result;
}

gchar *
qrcode_parse_texture (GdkTexture  *texture,
                      GError     **error)
{
    g_return_val_if_fail (texture != NULL, NULL);

    int w = gdk_texture_get_width (texture);
    int h = gdk_texture_get_height (texture);
    if (w <= 0 || h <= 0 ||
        (guint) w > MAX_QR_IMAGE_DIM || (guint) h > MAX_QR_IMAGE_DIM)
    {
        g_set_error (error, generic_error_gquark (), GENERIC_ERRCODE,
                     "Image dimensions out of range (%dx%d, max %ux%u).",
                     w, h, MAX_QR_IMAGE_DIM, MAX_QR_IMAGE_DIM);
        return NULL;
    }

    gsize stride = 0;
    gsize rgba_size = 0;
    gsize gray_size = 0;
    if (!checked_mul_size ((gsize) w, 4, &stride) ||
        !checked_mul_size ((gsize) h, stride, &rgba_size) ||
        !checked_mul_size ((gsize) w, (gsize) h, &gray_size))
    {
        g_set_error (error, generic_error_gquark (), GENERIC_ERRCODE,
                     "Image allocation size overflow.");
        return NULL;
    }
    guchar *rgba = g_malloc (rgba_size);
    /* gdk_texture_download() always emits BGRA in the host byte order. */
    gdk_texture_download (texture, rgba, stride);

    guchar *gray = g_malloc (gray_size);
    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            const guchar *px = &rgba[y * stride + x * 4];
            /* Channel order is B,G,R,A (cairo / GdkMemoryFormat default). */
            gray[y * w + x] = (guchar)(0.299 * px[2] + 0.587 * px[1] + 0.114 * px[0]);
        }
    }
    g_free (rgba);

    gchar *result = scan_grayscale_buffer (gray, (guint) w, (guint) h, error);
    g_free (gray);

    return result;
}
