#include <errno.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include "autostart.h"
#include "otpclient-application.h"
#include "version.h"

#define AUTOSTART_DESKTOP_FILE APPLICATION_ID ".desktop"

#ifdef IS_FLATPAK

#define PORTAL_BUS_NAME    "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define PORTAL_BACKGROUND  "org.freedesktop.portal.Background"
#define PORTAL_REQUEST     "org.freedesktop.portal.Request"

/* A portal that never answers must not strand the switch mid-flight. */
#define PORTAL_RESPONSE_TIMEOUT_SECONDS 60

typedef enum {
    BACKEND_UNKNOWN,
    BACKEND_AVAILABLE,
    BACKEND_MISSING
} BackendState;

static BackendState backend_state = BACKEND_UNKNOWN;

typedef struct {
    GDBusConnection     *bus;
    guint                signal_id;
    guint                timeout_id;
    gchar               *request_path;
    gboolean             wanted_autostart;
    gboolean             finished;
    /* Two owners, and either can be last: the pending RequestBackground call,
     * and the response side (the subscription plus the deadline). The Response
     * signal is allowed to arrive before the method reply does, which is the
     * whole reason we subscribe first, so the reply callback cannot assume the
     * request is still around. */
    gint                 refs;
    AutostartResultFunc  done;
    gpointer             user_data;
} AutostartRequest;

static void
autostart_request_unref (AutostartRequest *req)
{
    if (--req->refs > 0)
        return;

    g_clear_object (&req->bus);
    g_free (req->request_path);
    g_free (req);
}

static void
autostart_request_finish (AutostartRequest *req,
                          gboolean          granted)
{
    /* Whichever of the Response, the failed reply and the deadline gets here
     * first reports the outcome; the others find it already done. */
    if (req->finished)
        return;
    req->finished = TRUE;

    if (req->signal_id != 0)
    {
        g_dbus_connection_signal_unsubscribe (req->bus, req->signal_id);
        req->signal_id = 0;
    }
    /* on_response_timeout clears this before calling in, so removing the
     * currently dispatching source is not a case that arises. */
    g_clear_handle_id (&req->timeout_id, g_source_remove);

    if (req->done != NULL)
        req->done (granted, req->user_data);

    autostart_request_unref (req);
}

static gboolean
on_response_timeout (gpointer user_data)
{
    AutostartRequest *req = user_data;
    req->timeout_id = 0;
    g_warning ("The background portal did not answer within %d seconds",
               PORTAL_RESPONSE_TIMEOUT_SECONDS);
    autostart_request_finish (req, FALSE);
    return G_SOURCE_REMOVE;
}

static void
on_request_response (GDBusConnection *connection,
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

    AutostartRequest *req = user_data;

    guint32 response = 2;
    g_autoptr (GVariant) results = NULL;
    g_variant_get (parameters, "(u@a{sv})", &response, &results);

    /* 0 granted, 1 the user said no, 2 the request failed or was cancelled. */
    if (response != 0)
    {
        g_debug ("RequestBackground was refused (response %u)", response);
        autostart_request_finish (req, FALSE);
        return;
    }

    /* Checking the response code alone is not enough. Before x-d-p 1.21.2 a
     * failed autostart write still answered 0, with autostart false in the
     * results, and Debian stable and Ubuntu LTS ship older versions. */
    gboolean got_autostart = FALSE;
    if (results != NULL)
        g_variant_lookup (results, "autostart", "b", &got_autostart);

    autostart_request_finish (req, got_autostart == req->wanted_autostart);
}

static void
on_request_background_done (GObject      *source,
                            GAsyncResult *result,
                            gpointer      user_data)
{
    AutostartRequest *req = user_data;

    g_autoptr (GError) err = NULL;
    g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source),
                                                                result, &err);
    if (reply == NULL)
    {
        g_warning ("RequestBackground failed: %s", err->message);
        autostart_request_finish (req, FALSE);
    }
    else if (!req->finished)
    {
        /* The path is predictable and we subscribed to it before calling, which
         * is the whole point of the handle_token. Older portals could hand back
         * something else, in which case move the subscription. */
        const gchar *handle = NULL;
        g_variant_get (reply, "(&o)", &handle);
        if (g_strcmp0 (handle, req->request_path) != 0)
        {
            g_debug ("Portal returned an unexpected request path, re-subscribing");
            if (req->signal_id != 0)
                g_dbus_connection_signal_unsubscribe (req->bus, req->signal_id);
            g_free (req->request_path);
            req->request_path = g_strdup (handle);
            req->signal_id =
                g_dbus_connection_signal_subscribe (req->bus, PORTAL_BUS_NAME,
                                                    PORTAL_REQUEST, "Response",
                                                    req->request_path, NULL,
                                                    G_DBUS_SIGNAL_FLAGS_NONE,
                                                    on_request_response, req, NULL);
        }
    }

    autostart_request_unref (req);
}

/* /org/freedesktop/portal/desktop/request/<unique name, sanitised>/<token> */
static gchar *
build_request_path (GDBusConnection *bus,
                    const gchar     *token)
{
    const gchar *unique = g_dbus_connection_get_unique_name (bus);
    if (unique == NULL)
        return NULL;

    g_autofree gchar *sender = g_strdup (unique[0] == ':' ? unique + 1 : unique);
    for (gchar *p = sender; *p != '\0'; p++)
        if (*p == '.')
            *p = '_';

    return g_strdup_printf ("%s/request/%s/%s", PORTAL_OBJECT_PATH, sender, token);
}

static void
on_background_version_done (GObject      *source,
                            GAsyncResult *result,
                            gpointer      user_data)
{
    (void) user_data;

    g_autoptr (GError) err = NULL;
    g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source),
                                                                result, &err);
    if (reply == NULL)
    {
        /* x-d-p only exports the interface when a backend implements it, so
         * this is how "the desktop cannot do this" arrives. It is a distinct
         * outcome from the user saying no, which comes much later and as a
         * Response code. */
        g_debug ("No background portal on this session: %s", err->message);
        backend_state = BACKEND_MISSING;
        return;
    }

    backend_state = BACKEND_AVAILABLE;
}

void
autostart_init (void)
{
    if (backend_state != BACKEND_UNKNOWN)
        return;

    GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
    if (bus == NULL)
    {
        backend_state = BACKEND_MISSING;
        return;
    }

    /* Reading a property cannot prompt and cannot change anything, unlike
     * RequestBackground itself. */
    g_dbus_connection_call (bus, PORTAL_BUS_NAME, PORTAL_OBJECT_PATH,
                            "org.freedesktop.DBus.Properties", "Get",
                            g_variant_new ("(ss)", PORTAL_BACKGROUND, "version"),
                            G_VARIANT_TYPE ("(v)"), G_DBUS_CALL_FLAGS_NONE, -1,
                            NULL, on_background_version_done, NULL);

    g_object_unref (bus);
}

gboolean
autostart_is_supported (void)
{
    return backend_state != BACKEND_MISSING;
}

void
autostart_apply (OTPClientApplication *app,
                 gboolean              enable_autostart,
                 AutostartResultFunc   done,
                 gpointer              user_data)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (app));

    g_autoptr (GError) err = NULL;
    GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &err);
    if (bus == NULL)
    {
        g_warning ("Cannot reach the session bus: %s", err->message);
        if (done != NULL)
            done (FALSE, user_data);
        return;
    }

    g_autofree gchar *token = g_strdup_printf ("otpclient_%u", g_random_int ());
    g_autofree gchar *path = build_request_path (bus, token);
    if (path == NULL)
    {
        g_object_unref (bus);
        if (done != NULL)
            done (FALSE, user_data);
        return;
    }

    AutostartRequest *req = g_new0 (AutostartRequest, 1);
    req->refs = 2;   /* the pending call, and the response side */
    req->bus = bus;  /* transferred */
    req->request_path = g_steal_pointer (&path);
    req->wanted_autostart = enable_autostart;
    req->done = done;
    req->user_data = user_data;

    /* Subscribe before calling: the Response can arrive before the method
     * reply does. */
    req->signal_id =
        g_dbus_connection_signal_subscribe (bus, PORTAL_BUS_NAME,
                                            PORTAL_REQUEST, "Response",
                                            req->request_path, NULL,
                                            G_DBUS_SIGNAL_FLAGS_NONE,
                                            on_request_response, req, NULL);
    req->timeout_id = g_timeout_add_seconds (PORTAL_RESPONSE_TIMEOUT_SECONDS,
                                             on_response_timeout, req);

    GVariantBuilder opts;
    g_variant_builder_init (&opts, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add (&opts, "{sv}", "handle_token", g_variant_new_string (token));
    /* Shown by the portal. One string has to serve both callers, the tray one
     * and the autostart one, since it is the same request. */
    g_variant_builder_add (&opts, "{sv}", "reason",
                           g_variant_new_string (_("Stay available in the system tray, and start at login when enabled")));
    g_variant_builder_add (&opts, "{sv}", "autostart", g_variant_new_boolean (enable_autostart));
    /* Autostart joins the arguments with a plain space and does no quoting, so
     * every element has to stay free of spaces and shell metacharacters. */
    if (otpclient_application_get_start_minimized (app))
    {
        const gchar *argv[] = { "otpclient", "--start-minimized", NULL };
        g_variant_builder_add (&opts, "{sv}", "commandline",
                               g_variant_new_strv (argv, -1));
    }
    else
    {
        const gchar *argv[] = { "otpclient", NULL };
        g_variant_builder_add (&opts, "{sv}", "commandline",
                               g_variant_new_strv (argv, -1));
    }
    /* Must stay FALSE. A D-Bus-activatable entry is launched through
     * org.freedesktop.Application.Activate and the Exec argv is never used, so
     * --start-minimized would be silently dropped. */
    g_variant_builder_add (&opts, "{sv}", "dbus-activatable", g_variant_new_boolean (FALSE));

    g_dbus_connection_call (bus, PORTAL_BUS_NAME, PORTAL_OBJECT_PATH,
                            PORTAL_BACKGROUND, "RequestBackground",
                            g_variant_new ("(sa{sv})", "", &opts),
                            G_VARIANT_TYPE ("(o)"), G_DBUS_CALL_FLAGS_NONE, -1,
                            NULL, on_request_background_done, req);
}

void
autostart_ensure_background (OTPClientApplication *app)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (app));

    /* The autostart value has to be restated even though nobody here cares
     * about it: RequestBackground defaults it to FALSE, so omitting it would
     * delete the entry as a side effect of asking to stay alive. */
    autostart_apply (app, otpclient_application_get_autostart (app), NULL, NULL);
}

#else /* !IS_FLATPAK */

/* On the host the portal is not an option: it derives the app id from the
 * systemd unit, and a unit that does not start with app- gets "Autostart not
 * supported (no AppId detected)" from x-d-p 1.21.0 onwards. A terminal launch
 * has no such scope. So write the file, which is what every other host app
 * does, and choose the branch at compile time rather than falling back to it
 * after a portal failure. */

static gchar *
autostart_file_path (void)
{
    return g_build_filename (g_get_user_config_dir (), "autostart",
                             AUTOSTART_DESKTOP_FILE, NULL);
}

void
autostart_init (void)
{
}

gboolean
autostart_is_supported (void)
{
    return TRUE;
}

void
autostart_apply (OTPClientApplication *app,
                 gboolean              enable_autostart,
                 AutostartResultFunc   done,
                 gpointer              user_data)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (app));

    g_autofree gchar *path = autostart_file_path ();
    gboolean ok = TRUE;

    if (!enable_autostart)
    {
        if (g_unlink (path) != 0 && errno != ENOENT)
        {
            g_warning ("Could not remove %s: %s", path, g_strerror (errno));
            ok = FALSE;
        }
    }
    else
    {
        g_autofree gchar *dir = g_path_get_dirname (path);
        if (g_mkdir_with_parents (dir, 0700) != 0)
        {
            g_warning ("Could not create %s: %s", dir, g_strerror (errno));
            ok = FALSE;
        }
        else
        {
            g_autoptr (GKeyFile) kf = g_key_file_new ();
            g_key_file_set_string (kf, G_KEY_FILE_DESKTOP_GROUP,
                                   G_KEY_FILE_DESKTOP_KEY_TYPE, "Application");
            g_key_file_set_string (kf, G_KEY_FILE_DESKTOP_GROUP,
                                   G_KEY_FILE_DESKTOP_KEY_NAME, "OTPClient");
            g_key_file_set_string (kf, G_KEY_FILE_DESKTOP_GROUP,
                                   G_KEY_FILE_DESKTOP_KEY_EXEC,
                                   otpclient_application_get_start_minimized (app)
                                     ? "otpclient --start-minimized" : "otpclient");
            g_key_file_set_string (kf, G_KEY_FILE_DESKTOP_GROUP,
                                   G_KEY_FILE_DESKTOP_KEY_ICON, APPLICATION_ID);
            g_key_file_set_boolean (kf, G_KEY_FILE_DESKTOP_GROUP,
                                    G_KEY_FILE_DESKTOP_KEY_TERMINAL, FALSE);
            /* Otherwise some shells show a launch spinner that never resolves,
             * because a window that starts hidden never claims the token. */
            g_key_file_set_boolean (kf, G_KEY_FILE_DESKTOP_GROUP,
                                    G_KEY_FILE_DESKTOP_KEY_STARTUP_NOTIFY, FALSE);

            g_autoptr (GError) err = NULL;
            if (!g_key_file_save_to_file (kf, path, &err))
            {
                g_warning ("Could not write %s: %s", path, err->message);
                ok = FALSE;
            }
        }
    }

    if (done != NULL)
        done (ok, user_data);
}

void
autostart_ensure_background (OTPClientApplication *app)
{
    (void) app;
    /* Nothing kills a windowless app on the host, and writing an autostart file
     * here would turn "minimize to tray" into "start at login" behind the
     * user's back. */
}

#endif /* IS_FLATPAK */

static void
on_reassert_done (gboolean granted,
                  gpointer user_data)
{
    OTPClientApplication *app = user_data;

    if (!granted)
    {
        /* The key claimed an entry the desktop will not honour. Clear it and
         * take the file away too, so the user does not keep getting a launch
         * that is killed seconds later. */
        g_message ("Autostart is no longer permitted, removing the entry");
        otpclient_application_set_autostart (app, FALSE);
        autostart_apply (app, FALSE, NULL, NULL);
    }

    g_object_unref (app);
}

void
autostart_reassert (OTPClientApplication *app)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (app));

    if (otpclient_application_get_autostart (app))
    {
        autostart_apply (app, TRUE, on_reassert_done, g_object_ref (app));
        return;
    }

    autostart_ensure_background (app);
}
