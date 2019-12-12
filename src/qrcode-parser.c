#include <glib.h>
#include <zbar.h>
#include <png.h>
#include <glib/gstdio.h>
#include "gui-common.h"

typedef struct _image_data_t {
    guint width;
    guint height;
    guchar *raw_data;
} ImageData;

static gchar *set_data_from_png (const gchar *png_path,
                                 ImageData   *image_data);


gchar *
parse_qrcode (const gchar    *png_path,
              gchar         **otpauth_uri)
{

    zbar_image_scanner_t *scanner = zbar_image_scanner_create ();
    zbar_image_scanner_set_config (scanner, ZBAR_NONE, ZBAR_CFG_ENABLE, 1);

    ImageData *image_data = g_new0 (ImageData, 1);

    gchar *err_msg = set_data_from_png (png_path, image_data);
    if (err_msg != NULL) {
        g_free (image_data);
        zbar_image_scanner_destroy (scanner);
        return err_msg;
    }

    zbar_image_t *image = zbar_image_create ();
#ifdef ZBAR_OLD_LIB
    zbar_image_set_format (image, *(int*)"Y800");
#else
    zbar_image_set_format (image, zbar_fourcc ('Y','8','0','0'));
#endif
    zbar_image_set_size (image, image_data->width, image_data->height);
    zbar_image_set_data (image, image_data->raw_data, image_data->width * image_data->height, zbar_image_free_data);

    gint n = zbar_scan_image (scanner, image);
    if (n < 1) {
        zbar_image_destroy (image);
        zbar_image_scanner_destroy (scanner);
        g_free (image_data);
        return g_strdup ("Couldn't find a valid qrcode");
    }

    const zbar_symbol_t *symbol = zbar_image_first_symbol (image);
    for (; symbol; symbol = zbar_symbol_next (symbol)) {
        *otpauth_uri = secure_strdup (zbar_symbol_get_data (symbol));
    }

    zbar_image_destroy (image);
    zbar_image_scanner_destroy (scanner);

    g_free (image_data);

    return NULL;
}


static gchar *
set_data_from_png (const gchar   *png_path,
                   ImageData     *image_data)
{
    FILE *file = g_fopen (png_path, "rb");
    if (file == NULL) {
        return g_strdup ("Couldn't open the PNG file");
    }

    guchar sig[8];
    if (fread (sig, 1, 8, file) != 8) {
        fclose (file);
        return g_strdup ("Couldn't read signature from PNG file");
    }
    if (!png_check_sig (sig, 8)) {
        fclose (file);
        return g_strdup ("The file is not a PNG image");
    }

    png_structp png = png_create_read_struct (PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png == NULL) {
        fclose (file);
        return g_strdup ("png_create_read_struct failed");
    }

    png_infop info = png_create_info_struct (png);
    if (!info) {
        png_destroy_read_struct (&png, NULL, NULL);
        fclose (file);
        return g_strdup ("png_create_info_struct failed");
    }

    if (setjmp (png_jmpbuf (png))) {
        png_destroy_read_struct (&png, &info, NULL);
        fclose (file);
        return g_strdup ("setjmp failed");
    }

    png_init_io (png, file);
    png_set_sig_bytes (png, 8);
    png_read_info (png, info);

    gint color = png_get_color_type (png, info);
    gint bits = png_get_bit_depth (png, info);

    if (color & PNG_COLOR_TYPE_PALETTE) {
        png_set_palette_to_rgb (png);
    }

    if (color == PNG_COLOR_TYPE_GRAY && bits < 8) {
        png_set_expand_gray_1_2_4_to_8 (png);
    }

    if (bits == 16) {
        png_set_strip_16 (png);
    }

    if (color & PNG_COLOR_MASK_ALPHA) {
        png_set_strip_alpha (png);
    }

    if (color & PNG_COLOR_MASK_COLOR) {
        png_set_rgb_to_gray_fixed (png, 1, -1, -1);
    }

    image_data->width = (guint)png_get_image_width (png, info);
    image_data->height = (guint)png_get_image_height (png, info);
    image_data->raw_data = (guchar *)g_malloc0 (image_data->width * image_data->height);
    png_bytep rows[image_data->height];

    for (gint i = 0; i < image_data->height; i++) {
        rows[i] = image_data->raw_data + (image_data->width * i);
    }

    png_read_image (png, rows);

    png_read_end (png, NULL);

    png_destroy_read_struct (&png, &info, NULL);
    fclose (file);

    return NULL;
}