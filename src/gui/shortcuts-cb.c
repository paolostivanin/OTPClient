#include <gtk/gtk.h>
#include "message-dialogs.h"
#include "get-builder.h"
#include "data.h"

void
shortcuts_window_cb (GSimpleAction *simple    __attribute__((unused)),
                     GVariant      *parameter __attribute__((unused)),
                     gpointer       user_data)
{
    AppData *app_data = (AppData *)user_data;

    GtkBuilder *builder = get_builder_from_partial_path ("share/otpclient/shortcuts.ui");
    GtkWidget *overlay = GTK_WIDGET (gtk_builder_get_object (builder, "shortcuts-otpclient"));
    gtk_window_set_transient_for (GTK_WINDOW(overlay), GTK_WINDOW(app_data->main_window));
    gtk_widget_show (overlay);
    g_object_unref (builder);
}


void
show_kbs_cb_shortcut (GtkWidget *w __attribute__((unused)),
                      gpointer   user_data)
{
    shortcuts_window_cb (NULL, NULL, user_data);
}