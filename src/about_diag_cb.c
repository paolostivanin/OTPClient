#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include "version.h"
#include "data.h"

void
about_diag_cb (GSimpleAction *simple    __attribute__((unused)),
               GVariant      *parameter __attribute__((unused)),
               gpointer       user_data)
{
    AppData *app_data = (AppData *)user_data;

    const gchar *authors[] = {"Paolo Stivanin <info@paolostivanin.com>", NULL};
    const gchar *artists[] = {"Tobias Bernard (bertob) <https://tobiasbernard.com>", NULL};
    const gchar *partial_path = "share/icons/hicolor/scalable/apps/com.github.paolostivanin.OTPClient.svg";
    gchar *icon_abs_path = g_strconcat (INSTALL_PREFIX, "/", partial_path, NULL);

    GtkWidget *ab_diag = gtk_about_dialog_new ();
    gtk_window_set_transient_for (GTK_WINDOW(app_data->main_window), GTK_WINDOW(ab_diag));

    gtk_about_dialog_set_program_name (GTK_ABOUT_DIALOG(ab_diag), PROJECT_NAME);
    gtk_about_dialog_set_version (GTK_ABOUT_DIALOG(ab_diag), PROJECT_VER);
    gtk_about_dialog_set_copyright (GTK_ABOUT_DIALOG(ab_diag), "2017-2022");
    gtk_about_dialog_set_comments (GTK_ABOUT_DIALOG(ab_diag), _("Highly secure and easy to use GTK+ software for two-factor authentication that supports both Time-based One-time Passwords (TOTP) and HMAC-Based One-Time Passwords (HOTP)."));
    gtk_about_dialog_set_license_type (GTK_ABOUT_DIALOG(ab_diag), GTK_LICENSE_GPL_3_0);
    gtk_about_dialog_set_website (GTK_ABOUT_DIALOG(ab_diag), "https://github.com/paolostivanin/OTPClient");
    gtk_about_dialog_set_authors (GTK_ABOUT_DIALOG(ab_diag), authors);
    gtk_about_dialog_set_artists (GTK_ABOUT_DIALOG(ab_diag), artists);
    GdkPixbuf *logo = gdk_pixbuf_new_from_file (icon_abs_path, NULL);
    gtk_about_dialog_set_logo (GTK_ABOUT_DIALOG(ab_diag), logo);
    g_free (icon_abs_path);
    g_signal_connect (ab_diag, "response", G_CALLBACK (gtk_widget_destroy), NULL);

    gtk_widget_show_all (ab_diag);
}
