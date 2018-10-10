#include <gtk/gtk.h>
#include "message-dialogs.h"
#include "data.h"

void
shortcuts_window_cb (GSimpleAction *simple    __attribute__((unused)),
                     GVariant      *parameter __attribute__((unused)),
                     gpointer       user_data)
{
    AppData *app_data = (AppData *)user_data;

    if (gtk_get_major_version () == 3 && gtk_get_minor_version () < 20) {
        const gchar *msg = "Your GTK+ version is too old and does not support this feature.\n"
                           "To get a list of available keyboard shortcuts please have a look at:\n"
                           "https://github.com/paolostivanin/OTPClient/wiki/KeyboardShortcuts";
        show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
        return;
    }

#if GTK_CHECK_VERSION(3, 20, 0)
    GtkWidget *overlay = GTK_WIDGET (gtk_builder_get_object (app_data->builder, "shortcuts-otpclient"));
    gtk_window_set_transient_for (GTK_WINDOW(overlay), GTK_WINDOW(app_data->main_window));
    gtk_widget_show (overlay);
#endif
}