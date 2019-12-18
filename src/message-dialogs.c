#include <gtk/gtk.h>

void
show_message_dialog (GtkWidget      *parent,
                     const gchar    *message,
                     GtkMessageType  message_type)
{
    static GtkWidget *dialog = NULL;

    dialog = gtk_message_dialog_new (parent == NULL ? NULL : GTK_WINDOW(parent), GTK_DIALOG_MODAL, message_type, GTK_BUTTONS_OK, "%s", message);
    gtk_message_dialog_set_markup (GTK_MESSAGE_DIALOG(dialog), message);

    gtk_dialog_run (GTK_DIALOG(dialog));

    gtk_widget_destroy (dialog);
}


gboolean
get_confirmation_from_dialog (GtkWidget     *parent,
                              const gchar   *message)
{
    static GtkWidget *dialog = NULL;
    gboolean confirm;

    dialog = gtk_dialog_new_with_buttons ("Confirm", GTK_WINDOW (parent), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                          "OK", GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL,
                                           NULL);

    gtk_container_set_border_width (GTK_CONTAINER (dialog), 5);

    GtkWidget *content_area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    GtkWidget *label = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL (label), message);
    gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);
    gtk_container_add (GTK_CONTAINER (content_area), label);
    gtk_widget_show_all (dialog);

    gint result = gtk_dialog_run (GTK_DIALOG (dialog));
    switch (result) {
        case GTK_RESPONSE_OK:
            confirm = TRUE;
            break;
        case GTK_RESPONSE_CANCEL:
        default:
            confirm = FALSE;
            break;
    }
    gtk_widget_destroy (dialog);

    return confirm;
}