#include <glib.h>
#include <glib/gstdio.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <qrencode.h>
#include <string.h>
#include <unistd.h>
#include "qrcode-parser.h"
#include "gquarks.h"

static gchar *
make_tmp_path (const gchar *name,
               gchar      **dir_out)
{
    GError *err = NULL;
    gchar *dir = g_dir_make_tmp ("otpclient-qr-test-XXXXXX", &err);
    g_assert_no_error (err);
    g_assert_nonnull (dir);

    *dir_out = dir;
    return g_build_filename (dir, name, NULL);
}

static void
cleanup_tmp_path (gchar *dir,
                  gchar *path)
{
    g_unlink (path);
    g_rmdir (dir);
    g_free (path);
    g_free (dir);
}

static GdkTexture *
make_qr_texture (const gchar *payload)
{
    QRcode *qrcode = QRcode_encodeString (payload, 0, QR_ECLEVEL_M, QR_MODE_8, 1);
    g_assert_nonnull (qrcode);

    const int scale = 8;
    const int border = 4;
    const int size = (qrcode->width + border * 2) * scale;
    const gsize stride = (gsize) size * 4;
    const gsize len = stride * (gsize) size;
    guchar *pixels = g_malloc (len);
    memset (pixels, 0xff, len);

    for (int y = 0; y < qrcode->width; y++) {
        for (int x = 0; x < qrcode->width; x++) {
            if ((qrcode->data[y * qrcode->width + x] & 0x1) == 0)
                continue;

            const int px0 = (x + border) * scale;
            const int py0 = (y + border) * scale;
            for (int yy = 0; yy < scale; yy++) {
                guchar *row = pixels + ((gsize) (py0 + yy) * stride);
                for (int xx = 0; xx < scale; xx++) {
                    guchar *px = row + ((gsize) (px0 + xx) * 4);
                    px[0] = 0;
                    px[1] = 0;
                    px[2] = 0;
                    px[3] = 0xff;
                }
            }
        }
    }

    GBytes *bytes = g_bytes_new_take (pixels, len);
    GdkTexture *texture = gdk_memory_texture_new (size, size,
                                                  GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
                                                  bytes, stride);
    g_bytes_unref (bytes);

    QRcode_free (qrcode);
    return texture;
}

static void
test_valid_texture_qr (void)
{
    const gchar *payload = "otpauth://totp/Example:alice?secret=JBSWY3DPEHPK3PXP&issuer=Example";
    g_autoptr (GdkTexture) texture = make_qr_texture (payload);

    GError *err = NULL;
    gchar *uri = qrcode_parse_texture (texture, &err);
    g_assert_no_error (err);
    g_assert_cmpstr (uri, ==, payload);
    g_free (uri);
}

static void
test_corrupt_image_rejected (void)
{
    gchar *dir = NULL;
    gchar *path = make_tmp_path ("corrupt.png", &dir);

    GError *err = NULL;
    g_assert_true (g_file_set_contents (path, "not an image", -1, &err));
    g_assert_no_error (err);

    gchar *uri = qrcode_parse_image_file (path, &err);
    g_assert_null (uri);
    g_assert_nonnull (err);
    g_clear_error (&err);

    cleanup_tmp_path (dir, path);
}

static GdkPixbuf *
make_qr_pixbuf (const gchar *payload)
{
    QRcode *qrcode = QRcode_encodeString (payload, 0, QR_ECLEVEL_M, QR_MODE_8, 1);
    g_assert_nonnull (qrcode);

    const int scale = 8;
    const int border = 4;
    const int size = (qrcode->width + border * 2) * scale;

    GdkPixbuf *pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, size, size);
    g_assert_nonnull (pixbuf);

    const int rowstride = gdk_pixbuf_get_rowstride (pixbuf);
    const int channels = gdk_pixbuf_get_n_channels (pixbuf);
    guchar *pixels = gdk_pixbuf_get_pixels (pixbuf);

    for (int y = 0; y < size; y++)
        memset (pixels + (gsize) y * rowstride, 0xff, (gsize) size * channels);

    for (int y = 0; y < qrcode->width; y++) {
        for (int x = 0; x < qrcode->width; x++) {
            if ((qrcode->data[y * qrcode->width + x] & 0x1) == 0)
                continue;

            const int px0 = (x + border) * scale;
            const int py0 = (y + border) * scale;
            for (int yy = 0; yy < scale; yy++) {
                guchar *row = pixels + (gsize) (py0 + yy) * rowstride;
                for (int xx = 0; xx < scale; xx++) {
                    guchar *px = row + (gsize) (px0 + xx) * channels;
                    px[0] = 0;
                    px[1] = 0;
                    px[2] = 0;
                }
            }
        }
    }

    QRcode_free (qrcode);
    return pixbuf;
}

/* Skip the JPEG test gracefully when gdk-pixbuf can't actually produce a JPEG
 * in the current environment. Two failure modes show up in CI:
 *  - minimal images ship without a JPEG loader entirely (format not writable);
 *  - the writer is glycin-image-rs which wraps itself in bwrap, and bwrap
 *    can't spawn inside an unprivileged Docker container (no user namespaces),
 *    so the format is registered as writable but the save aborts at runtime.
 * Probe with a tiny save and skip on either failure. */
static gboolean
pixbuf_jpeg_save_works (void)
{
    g_autoptr (GdkPixbuf) probe = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, 8, 8);
    g_assert_nonnull (probe);
    gdk_pixbuf_fill (probe, 0xffffffff);

    gchar *tmpfile = NULL;
    GError *err = NULL;
    gint fd = g_file_open_tmp ("otpclient-jpeg-probe-XXXXXX.jpg", &tmpfile, &err);
    if (fd < 0) {
        g_clear_error (&err);
        g_free (tmpfile);
        return FALSE;
    }
    close (fd);

    gboolean ok = gdk_pixbuf_save (probe, tmpfile, "jpeg", &err, NULL);
    g_clear_error (&err);
    g_unlink (tmpfile);
    g_free (tmpfile);
    return ok;
}

static void
test_valid_jpeg_qr (void)
{
    if (!pixbuf_jpeg_save_works ()) {
        g_test_skip ("gdk-pixbuf JPEG writer unusable in this environment");
        return;
    }

    const gchar *payload = "otpauth://totp/Example:bob?secret=JBSWY3DPEHPK3PXP&issuer=Example";
    gchar *dir = NULL;
    gchar *path = make_tmp_path ("qr.jpg", &dir);

    g_autoptr (GdkPixbuf) pixbuf = make_qr_pixbuf (payload);

    GError *err = NULL;
    if (!gdk_pixbuf_save (pixbuf, path, "jpeg", &err, "quality", "95", NULL))
        g_error ("gdk_pixbuf_save(jpeg) failed: %s",
                 err != NULL ? err->message : "(no error message)");

    gchar *uri = qrcode_parse_image_file (path, &err);
    g_assert_no_error (err);
    g_assert_cmpstr (uri, ==, payload);
    g_free (uri);

    cleanup_tmp_path (dir, path);
}

/* Plain valid PNG with no QR (or other) symbol. Exercises the scan_grayscale_buffer
 * "no symbol found" path, which is also the outcome when an image contains only
 * a non-QR barcode given the scanner is configured to enable ZBAR_QRCODE only. */
static void
test_image_without_qr_rejected (void)
{
    gchar *dir = NULL;
    gchar *path = make_tmp_path ("plain.png", &dir);

    const int size = 128;
    g_autoptr (GdkPixbuf) pixbuf = gdk_pixbuf_new (GDK_COLORSPACE_RGB, FALSE, 8, size, size);
    g_assert_nonnull (pixbuf);

    const int rowstride = gdk_pixbuf_get_rowstride (pixbuf);
    const int channels = gdk_pixbuf_get_n_channels (pixbuf);
    guchar *pixels = gdk_pixbuf_get_pixels (pixbuf);
    for (int y = 0; y < size; y++)
        memset (pixels + (gsize) y * rowstride, 0xff, (gsize) size * channels);

    GError *err = NULL;
    g_assert_true (gdk_pixbuf_save (pixbuf, path, "png", &err, NULL));
    g_assert_no_error (err);

    gchar *uri = qrcode_parse_image_file (path, &err);
    g_assert_null (uri);
    g_assert_nonnull (err);
    g_clear_error (&err);

    cleanup_tmp_path (dir, path);
}

static void
test_oversized_texture_rejected (void)
{
    const int width = 4097;
    const int height = 1;
    const gsize stride = (gsize) width * 4;
    guchar *pixels = g_malloc0 (stride * height);
    GBytes *bytes = g_bytes_new_take (pixels, stride * height);
    g_autoptr (GdkTexture) texture = gdk_memory_texture_new (width, height,
                                                             GDK_MEMORY_B8G8R8A8_PREMULTIPLIED,
                                                             bytes, stride);
    g_bytes_unref (bytes);

    GError *err = NULL;
    gchar *uri = qrcode_parse_texture (texture, &err);
    g_assert_null (uri);
    g_assert_nonnull (err);
    g_clear_error (&err);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/qrcode/valid-texture", test_valid_texture_qr);
    g_test_add_func ("/qrcode/valid-jpeg", test_valid_jpeg_qr);
    g_test_add_func ("/qrcode/image-without-qr", test_image_without_qr_rejected);
    g_test_add_func ("/qrcode/corrupt-image", test_corrupt_image_rejected);
    g_test_add_func ("/qrcode/oversized-texture", test_oversized_texture_rejected);
    return g_test_run ();
}
