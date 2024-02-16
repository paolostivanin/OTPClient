#include <glib/gi18n.h>
#include "otpclient-application.h"
#include "otpclient-window.h"

struct _OTPClientWindow
{
    AdwApplicationWindow parent;

    GSettings *settings;
    GtkWidget *add_button;
    GtkWidget *remove_button;
    GtkWidget *reorder_button;
    GtkWidget *search_button;
    GtkWidget *search_bar;
    GtkWidget *search_entry;
    GtkWidget *lock_button;
    GtkWidget *settings_button;
};

G_DEFINE_FINAL_TYPE (OTPClientWindow, otpclient_window, ADW_TYPE_APPLICATION_WINDOW)

static void
search_func (OTPClientWindow *self,
             const gchar     *action_name,
             GVariant        *parameter)
{
    gboolean button_status = gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON(self->search_button));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(self->search_button), !button_status);
}

static void
search_text_changed (GtkEntry        *entry,
                     OTPClientWindow *win)
{
    const char *text = gtk_editable_get_text (GTK_EDITABLE(entry));
    if (text[0] == '\0')
        return;

    g_print ("%s\n", text);
}

static void
otpclient_window_dispose (GObject *object)
{
    OTPClientWindow *win = OTPCLIENT_WINDOW(object);
    //g_clear_object (&win->settings);

    gtk_widget_dispose_template (GTK_WIDGET (object), OTPCLIENT_TYPE_WINDOW);
    G_OBJECT_CLASS (otpclient_window_parent_class)->dispose (object);
}

static void
otpclient_window_init (OTPClientWindow *self)
{
    gtk_widget_init_template (GTK_WIDGET(self));

    //self->settings = g_settings_new ("com.github.paolostivanin.OTPClient");
}

static void
otpclient_window_class_init (OTPClientWindowClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = otpclient_window_dispose;

    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    gtk_widget_class_set_template_from_resource (widget_class, "/com/github/paolostivanin/OTPClient/ui/window.ui");
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, add_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, remove_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, reorder_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, search_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, search_bar);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, search_entry);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, lock_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, settings_button);

    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_q, GDK_CONTROL_MASK, "window.close", NULL);

    gtk_widget_class_install_action (widget_class, "window.search", NULL, (GtkWidgetActionActivateFunc)search_func);
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_f, GDK_CONTROL_MASK, "window.search", NULL);

    gtk_widget_class_bind_template_callback (klass, search_text_changed);
}

GtkWidget *
otpclient_window_new (OTPClientApplication *application)
{
    return g_object_new (OTPCLIENT_TYPE_WINDOW, "application", application, NULL);
}

