#include <glib/gi18n.h>
#include "otpclient-application.h"
#include "otpclient-window.h"

struct _OTPClientWindow
{
    AdwApplicationWindow application;
};

G_DEFINE_FINAL_TYPE (OTPClientWindow, otpclient_window, ADW_TYPE_APPLICATION_WINDOW)

static void
otpclient_window_init (OTPClientWindow *self)
{
    gtk_widget_init_template (GTK_WIDGET(self));
}

static void
otpclient_window_class_init (OTPClientWindowClass *class)
{
    gtk_widget_class_set_template_from_resource (GTK_WIDGET_CLASS(class), "/com/github/paolostivanin/OTPClient/ui/window.ui");
}

GtkWidget *
otpclient_window_new (OTPClientApplication *application)
{
    return g_object_new (OTPCLIENT_TYPE_WINDOW, "application", application, NULL);
}

