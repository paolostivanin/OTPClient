#include <glib/gi18n.h>
#include <qrencode.h>
#include "qr-display-dialog.h"

struct _QrDisplayDialog
{
    AdwDialog parent;
    GtkWidget *picture;
};

G_DEFINE_FINAL_TYPE (QrDisplayDialog, qr_display_dialog, ADW_TYPE_DIALOG)

static GdkTexture *
create_qr_texture (const gchar *data)
{
    QRcode *qrcode = QRcode_encodeString (data, 0, QR_ECLEVEL_M, QR_MODE_8, 1);
    if (qrcode == NULL)
        return NULL;

    gint scale = 8;
    gint size = qrcode->width * scale;
    gint stride = size * 4;  /* RGBA */
    guchar *pixels = g_malloc0 (stride * size);

    for (gint y = 0; y < qrcode->width; y++)
    {
        for (gint x = 0; x < qrcode->width; x++)
        {
            guchar module = qrcode->data[y * qrcode->width + x] & 1;
            guchar color = module ? 0x00 : 0xFF;

            for (gint sy = 0; sy < scale; sy++)
            {
                for (gint sx = 0; sx < scale; sx++)
                {
                    gint px = (y * scale + sy) * stride + (x * scale + sx) * 4;
                    pixels[px + 0] = color;  /* R */
                    pixels[px + 1] = color;  /* G */
                    pixels[px + 2] = color;  /* B */
                    pixels[px + 3] = 0xFF;   /* A */
                }
            }
        }
    }

    GBytes *bytes = g_bytes_new_take (pixels, stride * size);
    GdkTexture *texture = gdk_memory_texture_new (size, size,
                                                   GDK_MEMORY_R8G8B8A8,
                                                   bytes, stride);
    g_bytes_unref (bytes);
    QRcode_free (qrcode);

    return texture;
}

static void
qr_display_dialog_init (QrDisplayDialog *self)
{
    (void) self;
}

static void
qr_display_dialog_class_init (QrDisplayDialogClass *klass)
{
    (void) klass;
}

QrDisplayDialog *
qr_display_dialog_new (const gchar *otpauth_uri,
                        const gchar *label)
{
    QrDisplayDialog *self = g_object_new (QR_DISPLAY_TYPE_DIALOG,
                                          "title", label ? label : _("QR Code"),
                                          "content-width", 360,
                                          "content-height", 420,
                                          NULL);

    /* Build UI */
    GtkWidget *toolbar_view = adw_toolbar_view_new ();
    GtkWidget *header = adw_header_bar_new ();
    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);

    GtkWidget *clamp = adw_clamp_new ();
    gtk_widget_set_margin_start (clamp, 12);
    gtk_widget_set_margin_end (clamp, 12);
    gtk_widget_set_margin_top (clamp, 12);
    gtk_widget_set_margin_bottom (clamp, 12);

    GdkTexture *texture = create_qr_texture (otpauth_uri);
    if (texture != NULL)
    {
        self->picture = gtk_picture_new_for_paintable (GDK_PAINTABLE (texture));
        gtk_picture_set_content_fit (GTK_PICTURE (self->picture), GTK_CONTENT_FIT_CONTAIN);
        gtk_widget_set_size_request (self->picture, 256, 256);
        adw_clamp_set_child (ADW_CLAMP (clamp), self->picture);
        g_object_unref (texture);
    }
    else
    {
        GtkWidget *error_label = gtk_label_new (_("Failed to generate QR code"));
        adw_clamp_set_child (ADW_CLAMP (clamp), error_label);
    }

    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), clamp);
    adw_dialog_set_child (ADW_DIALOG (self), toolbar_view);

    return self;
}
