#include <glib/gi18n.h>
#include <zbar.h>
#include <png.h>
#include "qrcode-parser.h"
#include "gquarks.h"

/* Real-world QR code images for OTP enrollment are at most a few hundred
 * pixels per side. Cap the dimensions before any allocation: a malicious
 * source (PNG file, oversized clipboard texture) with width=height=65535
 * would otherwise request ~17 GB across the row pointer array, the per-row
 * buffers, and the grayscale buffer. g_malloc aborts on huge allocations,
 * which crashes the application — DoS via a single QR import. */
#define MAX_QR_IMAGE_DIM 4096u

static gchar *
scan_grayscale_buffer (const guchar  *gray,
                       guint          width,
                       guint          height,
                       GError       **error)
{
    zbar_image_scanner_t *scanner = zbar_image_scanner_create ();
    zbar_image_scanner_set_config (scanner, 0, ZBAR_CFG_ENABLE, 1);

    zbar_image_t *image = zbar_image_create ();
    zbar_image_set_format (image, zbar_fourcc ('Y', '8', '0', '0'));
    zbar_image_set_size (image, width, height);
    zbar_image_set_data (image, gray, width * height, NULL);

    gint n = zbar_scan_image (scanner, image);
    gchar *result = NULL;

    if (n > 0)
    {
        const zbar_symbol_t *symbol = zbar_image_first_symbol (image);
        if (symbol != NULL)
        {
            const gchar *data = zbar_symbol_get_data (symbol);
            if (data != NULL)
                result = g_strdup (data);
        }
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
load_png_image (const gchar  *filepath,
                guchar      **raw_data,
                guint        *width,
                guint        *height,
                GError      **error)
{
    FILE *fp = fopen (filepath, "rb");
    if (fp == NULL)
    {
        g_set_error (error, generic_error_gquark (), GENERIC_ERRCODE,
                     "Could not open file: %s", filepath);
        return FALSE;
    }

    png_structp png = png_create_read_struct (PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png == NULL)
    {
        fclose (fp);
        g_set_error (error, generic_error_gquark (), GENERIC_ERRCODE,
                     "Failed to create PNG read struct");
        return FALSE;
    }

    png_infop info = png_create_info_struct (png);
    if (info == NULL)
    {
        png_destroy_read_struct (&png, NULL, NULL);
        fclose (fp);
        g_set_error (error, generic_error_gquark (), GENERIC_ERRCODE,
                     "Failed to create PNG info struct");
        return FALSE;
    }

    if (setjmp (png_jmpbuf (png)))
    {
        png_destroy_read_struct (&png, &info, NULL);
        fclose (fp);
        g_set_error (error, generic_error_gquark (), GENERIC_ERRCODE,
                     "Error reading PNG file");
        return FALSE;
    }

    png_init_io (png, fp);
    png_read_info (png, info);

    *width = png_get_image_width (png, info);
    *height = png_get_image_height (png, info);
    if (*width == 0 || *height == 0 ||
        *width > MAX_QR_IMAGE_DIM || *height > MAX_QR_IMAGE_DIM)
    {
        png_destroy_read_struct (&png, &info, NULL);
        fclose (fp);
        g_set_error (error, generic_error_gquark (), GENERIC_ERRCODE,
                     "Image dimensions out of range (%ux%u, max %ux%u).",
                     *width, *height, MAX_QR_IMAGE_DIM, MAX_QR_IMAGE_DIM);
        return FALSE;
    }
    png_byte color_type = png_get_color_type (png, info);
    png_byte bit_depth = png_get_bit_depth (png, info);

    if (bit_depth == 16)
        png_set_strip_16 (png);
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb (png);
    if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
        png_set_expand_gray_1_2_4_to_8 (png);
    if (png_get_valid (png, info, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha (png);
    if (color_type == PNG_COLOR_TYPE_RGB ||
        color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_filler (png, 0xFF, PNG_FILLER_AFTER);
    if (color_type == PNG_COLOR_TYPE_GRAY ||
        color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
        png_set_gray_to_rgb (png);

    png_read_update_info (png, info);

    /* After the transformations above, every pixel is RGBA = 4 bytes. If
     * libpng disagrees (e.g. an unusual PNG that bypassed the filters),
     * bail out rather than indexing past the row at line `&row_pointers[y][x*4]`. */
    size_t expected_rowbytes = (size_t) (*width) * 4;
    if (png_get_rowbytes (png, info) < expected_rowbytes)
    {
        png_destroy_read_struct (&png, &info, NULL);
        fclose (fp);
        g_set_error (error, generic_error_gquark (), GENERIC_ERRCODE,
                     "Unexpected PNG row layout (got %zu bytes/row, expected at least %zu).",
                     (size_t) png_get_rowbytes (png, info), expected_rowbytes);
        return FALSE;
    }

    png_bytep *row_pointers = g_malloc (*height * sizeof (png_bytep));
    for (guint y = 0; y < *height; y++)
        row_pointers[y] = g_malloc (png_get_rowbytes (png, info));

    png_read_image (png, row_pointers);

    /* Convert to grayscale for zbar */
    *raw_data = g_malloc (*width * *height);
    for (guint y = 0; y < *height; y++)
    {
        for (guint x = 0; x < *width; x++)
        {
            png_bytep px = &(row_pointers[y][x * 4]);
            (*raw_data)[y * *width + x] = (guchar)(0.299 * px[0] + 0.587 * px[1] + 0.114 * px[2]);
        }
        g_free (row_pointers[y]);
    }
    g_free (row_pointers);

    png_destroy_read_struct (&png, &info, NULL);
    fclose (fp);

    return TRUE;
}

gchar *
qrcode_parse_image_file (const gchar  *filepath,
                          GError      **error)
{
    guchar *raw_data = NULL;
    guint width = 0, height = 0;

    if (!load_png_image (filepath, &raw_data, &width, &height, error))
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

    gsize stride = (gsize) w * 4;
    guchar *rgba = g_malloc ((gsize) h * stride);
    /* gdk_texture_download() always emits BGRA in the host byte order. */
    gdk_texture_download (texture, rgba, stride);

    guchar *gray = g_malloc ((gsize) w * (gsize) h);
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
