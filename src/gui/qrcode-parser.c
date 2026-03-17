#include <glib/gi18n.h>
#include <zbar.h>
#include <png.h>
#include "qrcode-parser.h"
#include "gquarks.h"

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

    zbar_image_scanner_t *scanner = zbar_image_scanner_create ();
    zbar_image_scanner_set_config (scanner, 0, ZBAR_CFG_ENABLE, 1);

    zbar_image_t *image = zbar_image_create ();
    zbar_image_set_format (image, zbar_fourcc ('Y', '8', '0', '0'));
    zbar_image_set_size (image, width, height);
    zbar_image_set_data (image, raw_data, width * height, NULL);

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
    g_free (raw_data);

    return result;
}
