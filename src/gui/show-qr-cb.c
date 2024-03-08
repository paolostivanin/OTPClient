#include <gtk/gtk.h>
#include <png.h>
#include <qrencode.h>
#include <glib/gstdio.h>
#include <glib/gi18n.h>
#include "data.h"
#include "parse-uri.h"
#include "get-builder.h"
#include "message-dialogs.h"

#define INCHES_PER_METER (100.0/2.54)
#define SIZE 3
#define MARGIN 2
#define DPI 72
#define PNG_OUT "/tmp/qrcode_otpclient.png"

static int    write_png       (const QRcode *qrcode);


void
show_qr_cb (GSimpleAction *simple    __attribute__((unused)),
            GVariant      *parameter __attribute__((unused)),
            gpointer       user_data)
{
    AppData *app_data = (AppData *)user_data;

    gchar *otpauth_uri = get_otpauth_uri (app_data, NULL);
    if (otpauth_uri == NULL) {
        show_message_dialog (app_data->main_window, "Error: a row must be selected in order to get the QR Code.", GTK_MESSAGE_ERROR);
        return;
    }
    QRcode *qr = QRcode_encodeString8bit ((const gchar *)otpauth_uri, 0, QR_ECLEVEL_H);
    write_png (qr);
    g_free (otpauth_uri);

    GtkBuilder *builder = get_builder_from_partial_path (UI_PARTIAL_PATH);
    GtkWidget *image = GTK_WIDGET(gtk_builder_get_object (builder, "qr_code_gtkimage_id"));
    GtkWidget *diag = GTK_WIDGET(gtk_builder_get_object (builder, "qr_code_diag_id"));

    GError *err = NULL;
    GdkPixbuf *pbuf = gdk_pixbuf_new_from_file (PNG_OUT, &err);
    if (err != NULL) {
        g_printerr ("Couldn't load the image: %s\n", err->message);
        return;
    }
    gtk_image_set_from_pixbuf (GTK_IMAGE(image), pbuf);
    gtk_widget_show_all (diag);

    gint response = gtk_dialog_run (GTK_DIALOG(diag));
    if (response == GTK_RESPONSE_OK) {
        gtk_widget_destroy (diag);
        g_object_unref (pbuf);
        g_object_unref (builder);
        if (g_unlink (PNG_OUT) == -1) {
            g_printerr ("%s\n", _("Couldn't unlink the PNG file."));
        }
    }
}


void
show_qr_cb_shortcut (GtkWidget *w __attribute__((unused)),
                     gpointer   user_data)
{
    show_qr_cb (NULL, NULL, user_data);
}


static int
write_png (const QRcode *qrcode)
{
    guint realwidth = (qrcode->width + MARGIN * 2) * SIZE;
    guchar *row = (guchar *)g_malloc0 ((size_t)((realwidth + 7) / 8));
    if (row == NULL) {
        g_printerr ("Failed to allocate memory.\n");
        return -1;
    }

    png_structp png_ptr = png_create_write_struct (PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (png_ptr == NULL) {
        g_printerr ("Failed to initialize PNG writer.\n");
        g_free (row);
        return -1;
    }

    png_infop info_ptr = png_create_info_struct (png_ptr);
    if (info_ptr == NULL) {
        g_printerr ("Failed to initialize PNG write.\n");
        g_free (row);
        return -1;
    }

    if (setjmp (png_jmpbuf(png_ptr))) {
        png_destroy_write_struct (&png_ptr, &info_ptr);
        g_printerr ("Failed to write PNG image.\n");
        g_free (row);
        return -1;
    }

    png_colorp palette = (png_colorp)g_malloc0 (sizeof (png_color) * 2);
    if (palette == NULL) {
        g_printerr ("Failed to allocate memory.\n");
        g_free (row);
        return -1;
    }

    guchar fg_color[4] = {0, 0, 0, 255};
    guchar bg_color[4] = {255, 255, 255, 255};
    png_byte alpha_values[2];

    palette[0].red   = fg_color[0];
    palette[0].green = fg_color[1];
    palette[0].blue  = fg_color[2];
    palette[1].red   = bg_color[0];
    palette[1].green = bg_color[1];
    palette[1].blue  = bg_color[2];
    alpha_values[0] = fg_color[3];
    alpha_values[1] = bg_color[3];
    png_set_PLTE(png_ptr, info_ptr, palette, 2);
    png_set_tRNS(png_ptr, info_ptr, alpha_values, 2, NULL);

    FILE *fp = fopen (PNG_OUT, "wb");
    if (fp == NULL) {
        g_printerr ("Failed to create file: %s\n", PNG_OUT);
        g_free (row);
        return -1;
    }
    png_init_io (png_ptr, fp);
    png_set_IHDR (png_ptr, info_ptr,
                 (guint)realwidth, (guint)realwidth,
                 1,
                 PNG_COLOR_TYPE_PALETTE,
                 PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_set_pHYs (png_ptr, info_ptr,
                 DPI * INCHES_PER_METER,
                 DPI * INCHES_PER_METER,
                 PNG_RESOLUTION_METER);
    png_write_info (png_ptr, info_ptr);

    memset (row, 0xff, (size_t)((realwidth + 7) / 8));
    for (gint y = 0; y < MARGIN * SIZE; y++) {
        png_write_row (png_ptr, row);
    }

    gint bit;
    guchar *q;
    guchar *p = qrcode->data;
    for (gint y = 0; y < qrcode->width; y++) {
        memset (row, 0xff, (size_t)((realwidth + 7) / 8));
        q = row + MARGIN * SIZE / 8;
        bit = 7 - (MARGIN * SIZE % 8);
        for (gint x = 0; x < qrcode->width; x++) {
            for (gint xx = 0; xx < SIZE; xx++) {
                *q ^= (*p & 1) << bit;
                bit--;
                if (bit < 0) {
                    q++;
                    bit = 7;
                }
            }
            p++;
        }
        for (gint yy = 0; yy < SIZE; yy++) {
            png_write_row (png_ptr, row);
        }
    }

    memset (row, 0xff, (size_t)((realwidth + 7) / 8));
    for (gint y = 0; y < MARGIN * SIZE; y++) {
        png_write_row (png_ptr, row);
    }

    png_write_end (png_ptr, info_ptr);
    png_destroy_write_struct (&png_ptr, &info_ptr);

    fclose (fp);
    g_free (row);
    g_free (palette);

    return 0;
}