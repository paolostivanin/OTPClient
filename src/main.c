#include <gtk/gtk.h>
#include "otpclient.h"

/*
 * gint s = g_date_time_get_second (g_date_time_new_now_local ());
 * g_print ("%d - %s\n", (s < 30) ? s+1 : s-29, get_totp("base32secret3232", 6));
 */

static void activate (GtkApplication *app, gpointer user_data);

static GdkPixbuf *create_logo (void);

gint
main (gint argc, gchar **argv)
{
    GtkApplication *app;
    gint status;

    GdkPixbuf *logo = create_logo ();
    if (logo != NULL)
        gtk_window_set_default_icon (logo);

    app = gtk_application_new ("org.gnome.otpclient", G_APPLICATION_FLAGS_NONE);
    g_set_application_name (APP_NAME);
    g_set_prgname (APP_NAME);
    g_signal_connect (app, "activate", G_CALLBACK (activate), logo);

    status = g_application_run (G_APPLICATION (app), argc, argv);

    g_object_unref (app);

    return status;
}


static GdkPixbuf *
create_logo ()
{
    GError *err = NULL;
    const gchar *my_icon = "/usr/share/icons/hicolor/128x128/apps/otpclient.png";

    GdkPixbuf *logo = gdk_pixbuf_new_from_file (my_icon, &err);
    if (err != NULL) {
        g_printerr ("%s\n", err->message);
        g_clear_error (&err);
    }

    return logo;
}
