#pragma once

#include <gtk/gtk.h>

#if GTK_CHECK_VERSION(4, 0, 0)
gint otpclient_dialog_run(GtkDialog *dialog);
gint otpclient_native_dialog_run(GtkNativeDialog *dialog);
gint otpclient_editable_get_text_length(GtkEditable *editable);
void otpclient_dialog_response(GtkDialog *dialog, gint response_id);
GtkWidget *otpclient_widget_get_toplevel(GtkWidget *widget);

#define gtk_dialog_run otpclient_dialog_run
#define gtk_native_dialog_run otpclient_native_dialog_run
#define gtk_editable_get_text_length otpclient_editable_get_text_length
#define gtk_dialog_response otpclient_dialog_response
#define gtk_widget_get_toplevel otpclient_widget_get_toplevel
#endif
