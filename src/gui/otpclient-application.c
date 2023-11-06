#include <gtk-4.0/gtk/gtk.h>
#include <glib/gi18n.h>
#include "otpclient-application.h"
#include "otpclient-window.h"
#include "version.h"

struct _OTPClientApplication
{
    AdwApplication  application;
    GtkWindow      *window;
    // config stuff
    gboolean show_next_otp;
    gboolean disable_notifications;
    gint search_column;
    gboolean auto_lock;
    gint inactivity_timeout;
    gboolean app_locked;
    gboolean use_dark_theme;
    gboolean is_reorder_active;
    gboolean use_secret_service;
};

G_DEFINE_TYPE (OTPClientApplication, otpclient_application, ADW_TYPE_APPLICATION)

static void otpclient_application_show_about      (GSimpleAction *simple,
                                                   GVariant      *parameter,
                                                   gpointer       user_data);

static void otpclient_application_quit            (GSimpleAction *simple,
                                                   GVariant      *parameter,
                                                   gpointer       user_data);

static const GActionEntry otpclient_application_entries[] = {
        { .name = "about", .activate = otpclient_application_show_about },
        { .name = "quit", .activate = otpclient_application_quit }
};

OTPClientApplication *
otpclient_application_new (void)
{
    return g_object_new (OTPCLIENT_TYPE_APPLICATION,
                         "application-id", "com.github.paolostivanin.OTPClient",
                         "flags", G_APPLICATION_DEFAULT_FLAGS,
                         "resource-base-path", "/com/github/paolostivanin/OTPClient",
                         NULL);

}

static void
set_accel_for_action (OTPClientApplication  *self,
                      const gchar           *detailed_action_name,
                      const gchar           *accel)
{
    const char *accels[] = { accel, NULL };
    gtk_application_set_accels_for_action (GTK_APPLICATION(self), detailed_action_name, accels);
}

static void
otpclient_application_show_about (GSimpleAction *simple,
                                  GVariant      *parameter,
                                  gpointer       user_data)
{
    static const gchar *developers[] = {
            "Paolo Stivanin <info@paolostivanin.com>",
            NULL
    };

    static const gchar *designers[] = {
            "Tobias Bernard (bertob) <https://tobiasbernard.com>",
            NULL
    };

    OTPClientApplication *self = OTPCLIENT_APPLICATION(user_data);

    adw_show_about_window (GTK_WINDOW(self->window),
                           "application-name", "OTPClient",
                           "application-icon", APPLICATION_ID,
                           "version", PROJECT_VER,
                           "copyright", "Copyright \xC2\xA9 2023 Paolo Stivanin",
                           "issue-url", "https://github.com/paolostivanin/OTPClient/issues",
                           "support-url", "https://github.com/paolostivanin/OTPClient/issues",
                           "website", "https://github.com/paolostivanin/OTPClient",
                           "license-type", GTK_LICENSE_GPL_3_0,
                           "developers", developers,
                           "designers", designers,
                           NULL);
}

static void
otpclient_application_quit (GSimpleAction *simple,
                            GVariant      *parameter,
                            gpointer       user_data)
{
    OTPClientApplication *self = OTPCLIENT_APPLICATION(user_data);
    gtk_window_destroy (self->window);
}

static void
otpclient_application_activate (GApplication *application)
{
    OTPClientApplication *self = OTPCLIENT_APPLICATION(application);
    gtk_window_present (GTK_WINDOW(self->window));
}

static void
otpclient_application_startup (GApplication *application)
{
    OTPClientApplication *self = OTPCLIENT_APPLICATION(application);

    g_action_map_add_action_entries (G_ACTION_MAP(self),
                                     otpclient_application_entries,
                                     G_N_ELEMENTS(otpclient_application_entries),
                                     self);

    set_accel_for_action (self, "app.quit", "<Control>q");
    set_accel_for_action (self, "app.about", "<Control>a");
    set_accel_for_action (self, "app.settings", "<Control>s");
    set_accel_for_action (self, "app.kb_shortcuts", "<Control>k");

    // set default values
    self->app_locked = FALSE;               // app is not locked when started
    self->show_next_otp = FALSE;            // next otp not shown by default
    self->disable_notifications = FALSE;    // notifications enabled by default
    self->search_column = 0;                // search by account
    self->auto_lock = FALSE;                // disabled by default
    self->inactivity_timeout = 0;           // never by default
    self->use_dark_theme = FALSE;           // light theme by default
    self->use_secret_service = TRUE;        // secret service enabled by default
    self->is_reorder_active = FALSE;        // when app is started, reorder is not set

    G_APPLICATION_CLASS (otpclient_application_parent_class)->startup (application);

    gtk_window_set_default_icon_name (APPLICATION_ID);
    self->window = GTK_WINDOW(otpclient_window_new (self));
}

static void
otpclient_application_class_init (OTPClientApplicationClass *klass)
{
    GApplicationClass *application_class = G_APPLICATION_CLASS(klass);
    application_class->activate = otpclient_application_activate;
    application_class->startup = otpclient_application_startup;
}

static void
otpclient_application_init (OTPClientApplication *self)
{
}