#include <glib/gi18n.h>
#include "lock-app.h"
#include "otpclient-application.h"
#include "otpclient-window.h"
#include "dialogs/password-dialog.h"
#include "db-common.h"

typedef struct
{
    OTPClientApplication *app;
    guint screensaver_watcher_id;
    guint inactivity_timer_id;
    gint64 last_user_activity;
} LockData;

static LockData *lock_data = NULL;

static void
on_unlock_password (const gchar *password,
                    gpointer     user_data)
{
    OTPClientApplication *app = OTPCLIENT_APPLICATION (user_data);
    DatabaseData *db_data = otpclient_application_get_db_data (app);

    if (password == NULL || db_data == NULL || db_data->key == NULL)
        return;

    if (g_strcmp0 (password, db_data->key) == 0)
    {
        lock_app_unlock (app);
    }
    else
    {
        /* Wrong password, show dialog again */
        PasswordDialog *dlg = password_dialog_new (PASSWORD_MODE_DECRYPT,
                                                   on_unlock_password,
                                                   app);
        GtkWidget *win = GTK_WIDGET (gtk_application_get_active_window (GTK_APPLICATION (app)));
        adw_dialog_present (ADW_DIALOG (dlg), win);
    }
}

void
lock_app_lock (OTPClientApplication *app)
{
    if (otpclient_application_get_app_locked (app))
        return;

    otpclient_application_set_app_locked (app, TRUE);

    GtkWindow *win = gtk_application_get_active_window (GTK_APPLICATION (app));
    if (win == NULL)
        return;

    /* Show password dialog to unlock */
    PasswordDialog *dlg = password_dialog_new (PASSWORD_MODE_DECRYPT,
                                               on_unlock_password,
                                               app);
    adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (win));
}

void
lock_app_unlock (OTPClientApplication *app)
{
    otpclient_application_set_app_locked (app, FALSE);

    if (lock_data != NULL)
        lock_data->last_user_activity = g_get_monotonic_time ();
}

void
lock_app_reset_inactivity (OTPClientApplication *app)
{
    (void) app;
    if (lock_data != NULL)
        lock_data->last_user_activity = g_get_monotonic_time ();
}

static void
on_screensaver_signal (GDBusConnection *connection,
                       const gchar     *sender_name,
                       const gchar     *object_path,
                       const gchar     *interface_name,
                       const gchar     *signal_name,
                       GVariant        *parameters,
                       gpointer         user_data)
{
    (void) connection;
    (void) sender_name;
    (void) object_path;
    (void) interface_name;
    (void) signal_name;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (user_data);
    gboolean active = FALSE;

    g_variant_get (parameters, "(b)", &active);
    if (active)
        lock_app_lock (app);
}

static gboolean
inactivity_check (gpointer user_data)
{
    OTPClientApplication *app = OTPCLIENT_APPLICATION (user_data);

    if (!otpclient_application_get_auto_lock (app))
        return G_SOURCE_CONTINUE;

    if (otpclient_application_get_app_locked (app))
        return G_SOURCE_CONTINUE;

    gint timeout = otpclient_application_get_inactivity_timeout (app);
    if (timeout <= 0)
        return G_SOURCE_CONTINUE;

    gint64 now = g_get_monotonic_time ();
    gint64 elapsed = (now - lock_data->last_user_activity) / G_USEC_PER_SEC;

    if (elapsed >= timeout)
        lock_app_lock (app);

    return G_SOURCE_CONTINUE;
}

void
lock_app_init_dbus_watchers (OTPClientApplication *app)
{
    if (lock_data != NULL)
        return;

    lock_data = g_new0 (LockData, 1);
    lock_data->app = app;
    lock_data->last_user_activity = g_get_monotonic_time ();

    GDBusConnection *bus = g_application_get_dbus_connection (G_APPLICATION (app));
    if (bus != NULL)
    {
        /* Subscribe to screensaver signals from various desktop environments */
        static const struct {
            const gchar *name;
            const gchar *path;
            const gchar *iface;
            const gchar *signal;
        } watchers[] = {
            { "org.gnome.ScreenSaver",       "/org/gnome/ScreenSaver",       "org.gnome.ScreenSaver",       "ActiveChanged" },
            { "org.cinnamon.ScreenSaver",     "/org/cinnamon/ScreenSaver",    "org.cinnamon.ScreenSaver",    "ActiveChanged" },
            { "org.freedesktop.ScreenSaver",  "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver", "ActiveChanged" },
            { "com.canonical.Unity",          "/com/canonical/Unity/Session", "com.canonical.Unity.Session", "Locked"        },
        };

        for (guint i = 0; i < G_N_ELEMENTS (watchers); i++)
        {
            g_dbus_connection_signal_subscribe (bus,
                                                watchers[i].name,
                                                watchers[i].iface,
                                                watchers[i].signal,
                                                watchers[i].path,
                                                NULL,
                                                G_DBUS_SIGNAL_FLAGS_NONE,
                                                on_screensaver_signal,
                                                app,
                                                NULL);
        }
    }

    lock_data->inactivity_timer_id = g_timeout_add_seconds (1, inactivity_check, app);
}

void
lock_app_cleanup (OTPClientApplication *app)
{
    (void) app;

    if (lock_data == NULL)
        return;

    if (lock_data->inactivity_timer_id != 0)
    {
        g_source_remove (lock_data->inactivity_timer_id);
        lock_data->inactivity_timer_id = 0;
    }

    g_free (lock_data);
    lock_data = NULL;
}

guint
lock_app_get_dbus_watcher_id (OTPClientApplication *app)
{
    (void) app;
    return lock_data ? lock_data->screensaver_watcher_id : 0;
}
