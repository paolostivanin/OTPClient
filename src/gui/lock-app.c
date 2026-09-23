#include <glib/gi18n.h>
#include "lock-app.h"
#include "otpclient-application.h"
#include "otpclient-window.h"
#include "dialogs/password-dialog.h"
#include "db-common.h"
#include "tray.h"

static const struct {
    const gchar *name;
    const gchar *path;
} screensavers[] = {
    { "org.gnome.ScreenSaver", "/org/gnome/ScreenSaver" },
    { "org.cinnamon.ScreenSaver", "/org/cinnamon/ScreenSaver" },
    { "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver" },
};

typedef struct
{
    guint watch_id;
    guint signal_id;
    guint64 revision;
    gchar *owner;
    gboolean active;
} ScreensaverState;

typedef struct
{
    OTPClientApplication *app;
    GDBusConnection *session_bus;
    GDBusConnection *system_bus;
    ScreensaverState screensavers[G_N_ELEMENTS (screensavers)];
    GCancellable *session_cancellable;
    guint unity_sub_id;
    gboolean session_locked;
    guint sleep_sub_id;
    guint inactivity_timer_id;
    gint64 last_user_activity;
} LockData;

static LockData *lock_data = NULL;

typedef struct
{
    guint index;
    guint64 revision;
    /* Also identifies this LockData lifetime, even after cleanup and re-init. */
    GCancellable *cancellable;
} ScreensaverQuery;

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

    /* An unlock submit is in flight: on_unlock_done loads the DB on success or
     * re-presents this dialog on failure. Don't pre-empt it. */
    if (otpclient_application_is_unlocking (app))
        return;

    /* Closed right after a successful unlock: the DB is open, nothing to do. */
    if (otpclient_application_is_db_unlocked (app))
        return;

    /* The user dismissed the prompt without unlocking (Escape, dialog X,
     * click-outside, or the parent window's close button). Drop to the locked
     * page instead of quitting or re-presenting, so the toolbar (Settings,
     * unlock) stays reachable (#467). */
    lock_app_enter_locked_state (app);
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
    g_signal_connect_object (dlg, "closed",
                             G_CALLBACK (on_unlock_dialog_closed),
                             app, 0);
}

static void
present_unlock_dialog_with_error (OTPClientApplication *app,
                                  const gchar          *error_message)
{
    GtkWindow *win = otpclient_application_get_window (app);
    if (win == NULL)
        return;

    PasswordDialog *dlg = password_dialog_new (PASSWORD_MODE_DECRYPT,
                                               on_unlock_password,
                                               app);
    /* A retry after a failed unlock arrives here with the reason the last
     * attempt failed; without it the prompt would simply reappear with no
     * indication that the password was wrong. */
    password_dialog_set_initial_error (dlg, error_message);
    lock_app_install_unlock_dialog_quit (dlg, app);
    adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (win));
}

void
lock_app_present_unlock_dialog (OTPClientApplication *app)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (app));
    if (otpclient_application_is_unlocking (app))
        return;
    present_unlock_dialog_with_error (app, NULL);
}

void
lock_app_present_unlock_dialog_with_error (OTPClientApplication *app,
                                           const gchar          *error_message)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (app));
    if (otpclient_application_is_unlocking (app))
        return;
    present_unlock_dialog_with_error (app, error_message);
}

static gboolean
on_unlock_password (const gchar  *current_password,
                    const gchar  *password,
                    gchar       **error_message,
                    gpointer      user_data)
{
    (void) current_password;
    OTPClientApplication *app = OTPCLIENT_APPLICATION (user_data);
    if (password == NULL)
        return FALSE;
    return otpclient_application_submit_unlock_password (app, password,
                                                         error_message);
}

void
lock_app_enter_locked_state (OTPClientApplication *app)
{
    if (otpclient_application_get_app_locked (app)) {
        if (otpclient_application_is_unlocking (app))
            otpclient_application_purge_secrets (app);
        return;
    }

    otpclient_application_set_app_locked (app, TRUE);

    GtkWindow *win = otpclient_application_get_window (app);
    if (win != NULL && OTPCLIENT_IS_WINDOW (win))
    {
        otpclient_window_secure_lock_cleanup (OTPCLIENT_WINDOW (win));
        otpclient_window_set_locked_indicator (OTPCLIENT_WINDOW (win), TRUE);
        otpclient_window_set_db_actions_enabled (OTPCLIENT_WINDOW (win), FALSE);
        otpclient_window_refresh_content_page (OTPCLIENT_WINDOW (win));
    }

    otpclient_application_purge_secrets (app);
}

void
lock_app_lock (OTPClientApplication *app)
{
    gboolean was_locked = otpclient_application_get_app_locked (app);

    lock_app_enter_locked_state (app);

    /* enter_locked_state is a no-op when already locked; only pop a fresh
     * unlock dialog when this call actually transitioned into the locked
     * state. */
    if (!was_locked)
        present_unlock_dialog_with_error (app, NULL);
}

void
lock_app_unlock (OTPClientApplication *app)
{
    otpclient_application_set_app_locked (app, FALSE);

    GtkWindow *win = otpclient_application_get_window (app);
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

gboolean
lock_app_get_session_locked (OTPClientApplication *app)
{
    return lock_data != NULL && lock_data->app == app && lock_data->session_locked;
}

static void
screensaver_set_active (guint index, gboolean active)
{
    lock_data->screensavers[index].active = active;
    gboolean locked = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS (screensavers); i++)
        locked |= lock_data->screensavers[i].active;

    if (lock_data->session_locked != locked) {
        lock_data->session_locked = locked;
#ifdef ENABLE_MINIMIZE_TO_TRAY
        otpclient_tray_notify_session_locked_changed (lock_data->app);
#endif
    }

    /* Track the desktop even when database Auto-Lock is disabled (#473). */
    if (active && otpclient_application_get_auto_lock (lock_data->app))
        lock_app_lock (lock_data->app);
}

static void
on_screensaver_active_changed (GDBusConnection *connection,
                               const gchar *sender_name,
                               const gchar *object_path,
                               const gchar *interface_name,
                               const gchar *signal_name,
                               GVariant *parameters,
                               gpointer user_data)
{
    (void) connection; (void) object_path; (void) interface_name; (void) signal_name;
    guint index = GPOINTER_TO_UINT (user_data);
    if (lock_data == NULL || !g_variant_is_of_type (parameters, G_VARIANT_TYPE ("(b)")))
        return;
    ScreensaverState *state = &lock_data->screensavers[index];
    if (g_strcmp0 (sender_name, state->owner) != 0)
        return;
    gboolean active;
    g_variant_get (parameters, "(b)", &active);
    state->revision++;
    screensaver_set_active (index, active);
}

static void
on_screensaver_active_read (GObject *source, GAsyncResult *result, gpointer user_data)
{
    ScreensaverQuery *query = user_data;
    g_autoptr (GError) error = NULL;
    g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
    if (lock_data != NULL && lock_data->session_cancellable == query->cancellable &&
        lock_data->screensavers[query->index].revision == query->revision) {
        if (reply != NULL) {
            gboolean active;
            g_variant_get (reply, "(b)", &active);
            screensaver_set_active (query->index, active);
        } else if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_debug ("Could not read %s state: %s", screensavers[query->index].name, error->message);
        }
    }
    g_object_unref (query->cancellable);
    g_free (query);
}

static void
on_screensaver_appeared (GDBusConnection *connection, const gchar *name,
                         const gchar *owner, gpointer user_data)
{
    (void) name;
    guint index = GPOINTER_TO_UINT (user_data);
    ScreensaverState *state = &lock_data->screensavers[index];
    g_free (state->owner);
    state->owner = g_strdup (owner);
    state->revision++;
    /* Subscribe before querying. A newer signal invalidates the query reply. */
    state->signal_id = g_dbus_connection_signal_subscribe (
        connection, owner, screensavers[index].name, "ActiveChanged", screensavers[index].path,
        NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_screensaver_active_changed, user_data, NULL);
    ScreensaverQuery *query = g_new0 (ScreensaverQuery, 1);
    query->index = index;
    query->revision = state->revision;
    query->cancellable = g_object_ref (lock_data->session_cancellable);
    g_dbus_connection_call (connection, owner, screensavers[index].path, screensavers[index].name,
        "GetActive", NULL, G_VARIANT_TYPE ("(b)"), G_DBUS_CALL_FLAGS_NO_AUTO_START,
        1000, query->cancellable, on_screensaver_active_read, query);
}

static void
on_screensaver_vanished (GDBusConnection *connection, const gchar *name, gpointer user_data)
{
    (void) name;
    guint index = GPOINTER_TO_UINT (user_data);
    ScreensaverState *state = &lock_data->screensavers[index];
    state->revision++;
    if (state->signal_id != 0) {
        g_dbus_connection_signal_unsubscribe (connection, state->signal_id);
        state->signal_id = 0;
    }
    g_clear_pointer (&state->owner, g_free);
    screensaver_set_active (index, FALSE);
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

    /* Only auto-lock on session lock when Auto-Lock is enabled, matching the
     * inactivity timer. Without this guard the app locks on every screen lock
     * even with Auto-Lock off (#460, a re-report of #279). */
    if (active && otpclient_application_get_auto_lock (app))
        lock_app_lock (app);
}

/* Only reachable off the system bus, which the sandbox does not have; see the
 * comment in lock_app_init_dbus_watchers. */
#ifndef IS_FLATPAK
static void
on_prepare_for_sleep (GDBusConnection *connection,
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
    gboolean preparing = FALSE;
    g_variant_get (parameters, "(b)", &preparing);

    /* Respect Auto-Lock here too (see on_screensaver_signal / #460). */
    if (preparing && otpclient_application_get_auto_lock (app))
        lock_app_lock (app);
}
#endif

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
    lock_data->session_cancellable = g_cancellable_new ();

    lock_data->session_bus = g_application_get_dbus_connection (G_APPLICATION (app));
    if (lock_data->session_bus != NULL)
    {
        for (guint i = 0; i < G_N_ELEMENTS (screensavers); i++) {
            lock_data->screensavers[i].watch_id = g_bus_watch_name_on_connection (
                lock_data->session_bus, screensavers[i].name, G_BUS_NAME_WATCHER_FLAGS_NONE,
                on_screensaver_appeared, on_screensaver_vanished, GUINT_TO_POINTER (i), NULL);
        }
        lock_data->unity_sub_id = g_dbus_connection_signal_subscribe (
            lock_data->session_bus, "com.canonical.Unity", "com.canonical.Unity.Session",
            "Locked", "/com/canonical/Unity/Session", NULL, G_DBUS_SIGNAL_FLAGS_NONE,
            on_screensaver_signal, app, NULL);
    }

    /* Lock on suspend needs the system bus, which a Flatpak sandbox does not
     * have. flatpak_run_add_system_dbus_args() binds /run/dbus/system_bus_socket
     * only for unrestricted apps or when the app declares a system-bus policy,
     * and flatpak_context_get_needs_system_bus_proxy() is literally
     * "g_hash_table_size (context->system_bus_policy) > 0". So without a
     * --system-talk-name there is no socket at all, not merely a policy that
     * rejects us: /run/dbus does not exist and g_bus_get_sync fails with "No
     * such file or directory".
     *
     * --system-talk-name=org.freedesktop.login1 is a hard flatpak-builder-lint
     * error, so this cannot be granted. Do not attempt the connection there:
     * it only produced a warning on every launch that no user could act on.
     * The four session screensaver watchers above are all reachable inside the
     * sandbox and cover the common case, someone walking away from the machine. */
#ifdef IS_FLATPAK
    g_debug ("Skipping suspend watch: the sandbox has no system bus");
#else
    GError *bus_error = NULL;
    lock_data->system_bus = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, &bus_error);
    if (lock_data->system_bus != NULL) {
        lock_data->sleep_sub_id = g_dbus_connection_signal_subscribe (
            lock_data->system_bus,
            "org.freedesktop.login1",
            "org.freedesktop.login1.Manager",
            "PrepareForSleep",
            "/org/freedesktop/login1",
            NULL,
            G_DBUS_SIGNAL_FLAGS_NONE,
            on_prepare_for_sleep,
            app,
            NULL);
    } else {
        /* Not a warning: a container or a session without logind is a
         * legitimate configuration, and the screensaver watchers still work. */
        g_debug ("Could not subscribe to suspend events: %s",
                 bus_error != NULL ? bus_error->message : "unknown error");
        g_clear_error (&bus_error);
    }
#endif

    lock_data->inactivity_timer_id = g_timeout_add_seconds (1, inactivity_check, app);
}

void
lock_app_cleanup (OTPClientApplication *app)
{
    (void) app;

    if (lock_data == NULL)
        return;

    g_cancellable_cancel (lock_data->session_cancellable);
    for (guint i = 0; i < G_N_ELEMENTS (screensavers); i++) {
        ScreensaverState *state = &lock_data->screensavers[i];
        if (state->watch_id != 0)
            g_bus_unwatch_name (state->watch_id);
        if (state->signal_id != 0)
            g_dbus_connection_signal_unsubscribe (lock_data->session_bus, state->signal_id);
        g_free (state->owner);
    }
    if (lock_data->unity_sub_id != 0)
        g_dbus_connection_signal_unsubscribe (lock_data->session_bus, lock_data->unity_sub_id);
    g_clear_object (&lock_data->session_cancellable);
    if (lock_data->system_bus != NULL && lock_data->sleep_sub_id != 0)
        g_dbus_connection_signal_unsubscribe (lock_data->system_bus,
                                              lock_data->sleep_sub_id);
    g_clear_object (&lock_data->system_bus);

    if (lock_data->inactivity_timer_id != 0)
    {
        g_source_remove (lock_data->inactivity_timer_id);
        lock_data->inactivity_timer_id = 0;
    }

    g_free (lock_data);
    lock_data = NULL;
}
