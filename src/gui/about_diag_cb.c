#include "gtk-compat.h"
#include <glib/gi18n.h>
#include "version.h"
#include "data.h"
#include "../common/macros.h"

static void
about_diag_response_cb (GtkDialog *dialog,
                        gint       response_id UNUSED,
                        gpointer   user_data UNUSED)
{
    gtk_window_destroy (GTK_WINDOW(dialog));
}

void
about_diag_cb (GSimpleAction *simple UNUSED,
               GVariant      *parameter UNUSED,
               gpointer       user_data)
{
    CAST_USER_DATA(AppData, app_data, user_data);

    const gchar *authors[] = {"Paolo Stivanin <info@paolostivanin.com>", NULL};
    const gchar *artists[] = {"Tobias Bernard (bertob) <https://tobiasbernard.com>", NULL};

    GtkWidget *ab_diag = gtk_about_dialog_new ();
    gtk_window_set_transient_for (GTK_WINDOW(ab_diag), GTK_WINDOW(app_data->main_window));

    gtk_about_dialog_set_program_name (GTK_ABOUT_DIALOG(ab_diag), PROJECT_NAME);
    gtk_about_dialog_set_version (GTK_ABOUT_DIALOG(ab_diag), PROJECT_VER);
    gtk_about_dialog_set_copyright (GTK_ABOUT_DIALOG(ab_diag), "2017-2024");
    gtk_about_dialog_set_comments (GTK_ABOUT_DIALOG(ab_diag), _("Highly secure and easy to use GTK+ software for two-factor authentication that supports both Time-based One-time Passwords (TOTP) and HMAC-Based One-Time Passwords (HOTP)."));
    gtk_about_dialog_set_license_type (GTK_ABOUT_DIALOG(ab_diag), GTK_LICENSE_GPL_3_0);
    gtk_about_dialog_set_website (GTK_ABOUT_DIALOG(ab_diag), "https://github.com/paolostivanin/OTPClient");
    gtk_about_dialog_set_authors (GTK_ABOUT_DIALOG(ab_diag), authors);
    gtk_about_dialog_set_artists (GTK_ABOUT_DIALOG(ab_diag), artists);
    gtk_about_dialog_set_logo_icon_name (GTK_ABOUT_DIALOG(ab_diag), "com.github.paolostivanin.OTPClient");
    g_signal_connect (ab_diag, "response", G_CALLBACK (about_diag_response_cb), NULL);

    gtk_window_present (GTK_WINDOW(ab_diag));
}
