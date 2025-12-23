#include "gtk-compat.h"

void
show_message_dialog (GtkWidget      *parent,
                     const gchar    *message,
                     GtkMessageType  message_type)
{
    static GtkWidget *dialog = NULL;

    dialog = gtk_message_dialog_new (parent == NULL ? NULL : GTK_WINDOW(parent), GTK_DIALOG_MODAL, message_type, GTK_BUTTONS_OK, "%s", message);
    gtk_message_dialog_set_markup (GTK_MESSAGE_DIALOG(dialog), message);

    gtk_dialog_run (GTK_DIALOG(dialog));

    gtk_window_destroy (GTK_WINDOW(dialog));
}


gboolean
get_confirmation_from_dialog (GtkWidget     *parent,
                              const gchar   *message)
{
    static GtkWidget *dialog = NULL;
    gboolean confirm;

    dialog = gtk_dialog_new_with_buttons ("Confirm", GTK_WINDOW(parent), GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
                                          "OK", GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL,
                                           NULL);

    GtkWidget *content_area = gtk_dialog_get_content_area (GTK_DIALOG(dialog));

    gtk_box_set_spacing(GTK_BOX(content_area), 8);
    gtk_widget_set_margin_top(content_area, 5);
    gtk_widget_set_margin_bottom(content_area, 5);
    gtk_widget_set_margin_start(content_area, 5);
    gtk_widget_set_margin_end(content_area, 5);

    GtkWidget *label = gtk_label_new (NULL);
    gtk_label_set_markup (GTK_LABEL(label), message);
    gtk_label_set_justify (GTK_LABEL(label), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(content_area), label);
    gtk_window_present (GTK_WINDOW(dialog));

    gint result = gtk_dialog_run (GTK_DIALOG(dialog));
    switch (result) {
        case GTK_RESPONSE_OK:
            confirm = TRUE;
            break;
        case GTK_RESPONSE_CANCEL:
        default:
            confirm = FALSE;
            break;
    }
    gtk_window_destroy (GTK_WINDOW(dialog));

    return confirm;
}
