#include <gtk/gtk.h>

void
show_message_dialog (GtkWidget      *parent,
                     const gchar    *message,
                     GtkMessageType  message_type)
{
    static GtkWidget *dialog = NULL;

    if (parent == NULL) {
        dialog = gtk_message_dialog_new (NULL, GTK_DIALOG_MODAL, message_type, GTK_BUTTONS_OK, NULL);
    }
    else {
        dialog = gtk_message_dialog_new (GTK_WINDOW (parent), GTK_DIALOG_MODAL, message_type, GTK_BUTTONS_OK, NULL);
    }

    gtk_message_dialog_set_markup (GTK_MESSAGE_DIALOG (dialog), message);

    gtk_dialog_run (GTK_DIALOG (dialog));

    gtk_widget_destroy (dialog);
}