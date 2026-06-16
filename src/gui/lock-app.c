#include <glib/gi18n.h>
#include "lock-app.h"
#include "otpclient-application.h"
#include "otpclient-window.h"
#include "dialogs/password-dialog.h"
#include "db-common.h"

typedef struct
{
    OTPClientApplication *app;
    GDBusConnection *bus;
    guint dbus_sub_ids[4];
    guint num_dbus_subs;
    guint inactivity_timer_id;
    gint64 last_user_activity;
} LockData;

static LockData *lock_data = NULL;

static gboolean on_unlock_password (const gchar  *current_password,
                                    const gchar  *password,
                                    gchar       **error_message,
                                    gpointer      user_data);

static void
on_unlock_dialog_closed (AdwDialog *dialog,
                         gpointer   user_data)
{
    (void) dialog;
    OTPClientApplication *app = OTPCLIENT_APPLICATION (user_data);

    /* set_can_close(FALSE) blocks user-initiated dismissal, but the dialog can
     * still close on parent-window teardown or programmatic close. If the app
     * is still locked when that happens, re-present a fresh dialog so the user
     * can't end up with the locked indicator on and no way to unlock. */
    if (otpclient_application_get_app_locked (app))
        lock_app_present_unlock_dialog (app);
}

static void
on_unlock_dialog_quit_requested (PasswordDialog       *dlg,
                                 OTPClientApplication *app)
{
    (void) dlg;
    g_application_quit (G_APPLICATION (app));
}

void
lock_app_install_unlock_dialog_quit (PasswordDialog       *dlg,
                                     OTPClientApplication *app)
{
    g_return_if_fail (PASSWORD_IS_DIALOG (dlg));
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (app));

    password_dialog_set_locked_mode (dlg);
    g_signal_connect_object (dlg, "quit-requested",
                             G_CALLBACK (on_unlock_dialog_quit_requested),
                             app, 0);
}

static void
present_unlock_dialog (OTPClientApplication *app)
{
    GtkWindow *win = gtk_application_get_active_window (GTK_APPLICATION (app));
    if (win == NULL)
        return;

    PasswordDialog *dlg = password_dialog_new (PASSWORD_MODE_DECRYPT,
                                               on_unlock_password,
                                               app);
    lock_app_install_unlock_dialog_quit (dlg, app);
    g_signal_connect (dlg, "closed", G_CALLBACK (on_unlock_dialog_closed), app);
    adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (win));
}

void
lock_app_present_unlock_dialog (OTPClientApplication *app)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (app));
    present_unlock_dialog (app);
}

static gboolean
on_unlock_password (const gchar  *current_password,
                    const gchar  *password,
                    gchar       **error_message,
                    gpointer      user_data)
{
    (void) current_password;
    OTPClientApplication *app = OTPCLIENT_APPLICATION (user_data);
    DatabaseData *db_data = otpclient_application_get_db_data (app);

    if (password == NULL || db_data == NULL || db_data->key == NULL)
        return FALSE;

    gsize pwd_len = strlen (password);
    gsize key_len = strlen (db_data->key);
    volatile guchar result = (pwd_len != key_len);
    gsize cmp_len = (pwd_len < key_len) ? pwd_len : key_len;
    for (gsize i = 0; i < cmp_len; i++)
        result |= ((const volatile guchar *)password)[i] ^ ((const volatile guchar *)db_data->key)[i];
    if (result == 0) {
        lock_app_unlock (app);
        return TRUE;
    }

    if (error_message != NULL)
        *error_message = g_strdup (_("Password is incorrect"));
    return FALSE;
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

    if (OTPCLIENT_IS_WINDOW (win))
    {
        /* Persist any deferred HOTP counter advances while we still hold the key. */
        otpclient_window_flush_pending_writes (OTPCLIENT_WINDOW (win));
        otpclient_window_clear_clipboard_now (OTPCLIENT_WINDOW (win));
        otpclient_window_clear_displayed_otps (OTPCLIENT_WINDOW (win));
        otpclient_window_set_locked_indicator (OTPCLIENT_WINDOW (win), TRUE);
        otpclient_window_set_db_actions_enabled (OTPCLIENT_WINDOW (win), FALSE);
        otpclient_window_refresh_content_page (OTPCLIENT_WINDOW (win));
    }

    present_unlock_dialog (app);
}

void
lock_app_unlock (OTPClientApplication *app)
{
    otpclient_application_set_app_locked (app, FALSE);

    GtkWindow *win = gtk_application_get_active_window (GTK_APPLICATION (app));
    if (win != NULL && OTPCLIENT_IS_WINDOW (win))
    {
        otpclient_window_set_locked_indicator (OTPCLIENT_WINDOW (win), FALSE);
        otpclient_window_set_db_actions_enabled (OTPCLIENT_WINDOW (win), TRUE);
        otpclient_window_refresh_content_page (OTPCLIENT_WINDOW (win));
    }

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

    lock_data->bus = g_application_get_dbus_connection (G_APPLICATION (app));
    if (lock_data->bus != NULL)
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
            lock_data->dbus_sub_ids[lock_data->num_dbus_subs++] =
                g_dbus_connection_signal_subscribe (lock_data->bus,
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

    if (lock_data->bus != NULL)
    {
        for (guint i = 0; i < lock_data->num_dbus_subs; i++)
            g_dbus_connection_signal_unsubscribe (lock_data->bus, lock_data->dbus_sub_ids[i]);
    }

    if (lock_data->inactivity_timer_id != 0)
    {
        g_source_remove (lock_data->inactivity_timer_id);
        lock_data->inactivity_timer_id = 0;
    }

    g_free (lock_data);
    lock_data = NULL;
}
