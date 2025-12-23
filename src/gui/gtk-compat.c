#include "gtk-compat.h"

#if GTK_CHECK_VERSION(4, 0, 0)
typedef struct {
    GMainLoop *loop;
    gint response;
} DialogRunData;

static void
dialog_response_cb(GtkDialog *dialog,
                   gint response,
                   gpointer user_data)
{
    DialogRunData *data = user_data;
    data->response = response;
    g_main_loop_quit(data->loop);
    gtk_window_close(GTK_WINDOW(dialog));
}

gint
otpclient_dialog_run(GtkDialog *dialog)
{
    DialogRunData data = {
        .loop = g_main_loop_new(NULL, FALSE),
        .response = GTK_RESPONSE_NONE,
    };

    g_signal_connect(dialog, "response", G_CALLBACK(dialog_response_cb), &data);
    gtk_window_present(GTK_WINDOW(dialog));
    g_main_loop_run(data.loop);
    g_signal_handlers_disconnect_by_func(dialog, dialog_response_cb, &data);
    g_main_loop_unref(data.loop);

    return data.response;
}

static void
native_dialog_response_cb(GtkNativeDialog *dialog,
                          gint response,
                          gpointer user_data)
{
    DialogRunData *data = user_data;
    data->response = response;
    g_main_loop_quit(data->loop);
    gtk_native_dialog_hide(dialog);
}

gint
otpclient_native_dialog_run(GtkNativeDialog *dialog)
{
    DialogRunData data = {
        .loop = g_main_loop_new(NULL, FALSE),
        .response = GTK_RESPONSE_NONE,
    };

    g_signal_connect(dialog, "response", G_CALLBACK(native_dialog_response_cb), &data);
    gtk_native_dialog_show(dialog);
    g_main_loop_run(data.loop);
    g_signal_handlers_disconnect_by_func(dialog, native_dialog_response_cb, &data);
    g_main_loop_unref(data.loop);

    return data.response;
}

gint
otpclient_editable_get_text_length(GtkEditable *editable)
{
    const gchar *text = gtk_editable_get_text(editable);

    return text == NULL ? 0 : (gint)g_utf8_strlen(text, -1);
}

void
otpclient_dialog_response(GtkDialog *dialog, gint response_id)
{
    gtk_window_close(GTK_WINDOW(dialog));
    g_signal_emit_by_name(dialog, "response", response_id);
}

GtkWidget *
otpclient_widget_get_toplevel(GtkWidget *widget)
{
    GtkRoot *root = gtk_widget_get_root(widget);

    return root != NULL ? GTK_WIDGET(root) : NULL;
}

#endif
