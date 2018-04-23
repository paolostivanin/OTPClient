#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <zbar.h>
#include <png.h>
#include <gcrypt.h>
#include "imports.h"
#include "message-dialogs.h"
#include "common.h"
#include "add-common.h"

typedef struct _image_data_t {
    guint width;
    guint height;
    guchar *raw_data;
} ImageData;

static gchar *parse_qrcode    (const gchar   *png_path,
                               gchar        **otpauth_uri);

static gchar *get_data        (const gchar *png_path,
                               ImageData   *image_data);


void
screenshot_cb (GSimpleAction *simple    __attribute__((unused)),
               GVariant      *parameter __attribute__((unused)),
               gpointer       user_data)
{
    ImportData *import_data = (ImportData *)user_data;

    GError *err = NULL;
    GDBusConnection *connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &err);
    if (err != NULL) {
        show_message_dialog (import_data->main_window, err->message, GTK_MESSAGE_ERROR);
        g_clear_error (&err);
        return;
    }

    const gchar *interface = "org.gnome.Shell.Screenshot";
    const gchar *object_path = "/org/gnome/Shell/Screenshot";

    GVariant *res = g_dbus_connection_call_sync (connection, interface, object_path, interface,
                                                 "SelectArea", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, G_MAXINT, NULL, &err);
    if (err != NULL) {
        show_message_dialog (import_data->main_window, err->message, GTK_MESSAGE_ERROR);
        g_object_unref (connection);
        g_clear_error (&err);
        return;
    }

    gint x, y, width, height;
    g_variant_get (res, "(iiii)", &x, &y, &width, &height);
    g_variant_unref (res);

    gchar *filename = g_build_filename (g_get_tmp_dir (), "qrcode.png", NULL);
    res = g_dbus_connection_call_sync (connection, interface, object_path, interface,
                                       "ScreenshotArea", g_variant_new ("(iiiibs)", x, y, width, height, TRUE, filename),
                                       NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
    if (err != NULL) {
        show_message_dialog (import_data->main_window, err->message, GTK_MESSAGE_ERROR);
        g_clear_error (&err);
        g_free (filename);
        g_object_unref (connection);
        return;
    }

    gboolean success;
    gchar *out_filename;
    g_variant_get (res, "(bs)", &success, &out_filename);
    if (!success) {
        show_message_dialog (import_data->main_window, "Failed to get screenshot using ScreenshotArea", GTK_MESSAGE_ERROR);
        g_variant_unref (res);
        g_free (filename);
        g_object_unref (connection);
        return;
    }

    gchar *otpauth_uri = NULL;
    gchar *err_msg = parse_qrcode (out_filename, &otpauth_uri);
    if (err_msg != NULL) {
        show_message_dialog (import_data->main_window, err_msg, GTK_MESSAGE_ERROR);
        g_free (err_msg);
        g_variant_unref (res);
        g_free (filename);
        g_object_unref (connection);
        return;
    }

    g_unlink (filename);

    err_msg = add_data_to_db (otpauth_uri, import_data);
    if (err_msg != NULL) {
        show_message_dialog (import_data->main_window, err_msg, GTK_MESSAGE_ERROR);
        g_free (err_msg);
        // no need to return here as we have to clean-up also the following stuff
    } else {
        show_message_dialog (import_data->main_window, "QRCode successfully imported from the screenshot", GTK_MESSAGE_INFO);
    }

    gcry_free (otpauth_uri);
    g_variant_unref (res);
    g_free (filename);
    g_object_unref (connection);
}


static gchar *
parse_qrcode (const gchar    *png_path,
              gchar         **otpauth_uri)
{

    zbar_image_scanner_t *scanner = zbar_image_scanner_create ();
    zbar_image_scanner_set_config (scanner, ZBAR_NONE, ZBAR_CFG_ENABLE, 1);

    ImageData *image_data = g_new0 (ImageData, 1);

    gchar *err_msg = get_data (png_path, image_data);
    if (err_msg != NULL) {
        g_free (image_data);
        zbar_image_scanner_destroy (scanner);
        return err_msg;
    }

    zbar_image_t *image = zbar_image_create ();
    zbar_image_set_format (image, zbar_fourcc ('Y','8','0','0'));
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
get_data (const gchar   *png_path,
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