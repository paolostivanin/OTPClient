#include <gtk/gtk.h>
#include <gcrypt.h>
#include "common.h"

static void password_cb (GtkWidget *entry, gpointer *pwd);


gchar *
prompt_for_password (GtkWidget *main_window)
{
    GtkDialogFlags flags = GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT;
    GtkWidget *dialog = gtk_dialog_new_with_buttons ("Password",
                                                     GTK_WINDOW (main_window),
                                                     flags,
                                                     "Cancel", GTK_RESPONSE_CLOSE,
                                                     "OK", GTK_RESPONSE_ACCEPT,
                                                     NULL);

    gtk_window_set_position (GTK_WINDOW (dialog), GTK_WIN_POS_CENTER);

    gchar *pwd = NULL;
    GtkWidget *entry = gtk_entry_new ();
    g_signal_connect (entry, "activate", G_CALLBACK (password_cb), &pwd);

    set_icon_to_entry (entry, "dialog-password-symbolic", "Show password");

    GtkWidget *content_area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    gtk_container_add (GTK_CONTAINER (content_area), entry);

    gtk_widget_show_all (dialog);

    gint ret = gtk_dialog_run (GTK_DIALOG (dialog));
    switch (ret) {
        case GTK_RESPONSE_ACCEPT:
            password_cb (entry, (gpointer *)&pwd);
            break;
        case GTK_RESPONSE_CLOSE:
            break;
        default:
            break;
    }
    gtk_widget_destroy (dialog);

    return pwd;
}


static void
password_cb (GtkWidget  *entry,
             gpointer   *pwd)
{
    const gchar *text = gtk_entry_get_text (GTK_ENTRY (entry));
    *pwd = gcry_calloc_secure (strlen (text) + 1, 1);
    strncpy (*pwd, text, strlen (text) + 1);
    GtkWidget *top_level = gtk_widget_get_toplevel (entry);
    gtk_dialog_response (GTK_DIALOG (top_level), GTK_RESPONSE_CLOSE);
}
