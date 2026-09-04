#include <errno.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include "autostart.h"
#include "autostart-policy.h"
#include "otpclient-application.h"
#include "version.h"

#define AUTOSTART_DESKTOP_FILE APPLICATION_ID ".desktop"

/* Bumped by every autostart_apply, before anything can fail, so it tracks the
 * most recent statement of intent rather than the most recent successful call.
 * An answer stamped with an older value is about a state that has since been
 * restated; see AutostartResult.superseded. */
static guint autostart_generation = 0;

static void autostart_reconcile (OTPClientApplication  *app,
                                 gboolean               wanted_autostart,
                                 const AutostartResult *result);

/* The single exit for every answer, however it was arrived at, so that no path
 * can report an outcome without the two keys having been squared with it first.
 * app may be NULL if the application went away mid-request, which leaves
 * nothing to reconcile but still owes the callback its call. */
static void
autostart_deliver (OTPClientApplication  *app,
                   gboolean               wanted_autostart,
                   guint                  generation,
                   const AutostartResult *result,
                   AutostartResultFunc    done,
                   gpointer               user_data)
{
    AutostartResult stamped = *result;
    stamped.superseded = (generation != autostart_generation);

    if (!stamped.superseded && app != NULL)
        autostart_reconcile (app, wanted_autostart, &stamped);

    if (done != NULL)
        done (&stamped, user_data);
}

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
    guint                generation;
    gboolean             finished;
    /* Two owners, and either can be last: the pending RequestBackground call,
     * and the response side (the subscription plus the deadline). The Response
     * signal is allowed to arrive before the method reply does, which is the
     * whole reason we subscribe first, so the reply callback cannot assume the
     * request is still around. */
    gint                 refs;
    /* Weak, so a request the user will never see the result of cannot keep the
     * application alive until the portal gets round to answering. */
    GWeakRef             app_ref;
    AutostartResultFunc  done;
    gpointer             user_data;
} AutostartRequest;

/* The intent behind a call that arrived while another was in flight. Only ever
 * one: a second one displaces it, because two held-back requests would be two
 * statements of the same thing and only the newer is true. */
typedef struct {
    GWeakRef             app_ref;
    gboolean             wanted_autostart;
    guint                generation;
    AutostartResultFunc  done;
    gpointer             user_data;
} AutostartPending;

/* Exactly one request may be talking to the portal. Overlapping calls used to
 * be allowed on the theory that the newest answer could just be preferred, but
 * the answer is not the part that matters: both requests reach the portal, and
 * the desktop applies whichever it finishes last. */
static AutostartRequest *autostart_inflight = NULL;
static AutostartPending *autostart_queued   = NULL;

static void autostart_start_request (OTPClientApplication *app,
                                     gboolean              enable_autostart,
                                     guint                 generation,
                                     AutostartResultFunc   done,
                                     gpointer              user_data);

static void
autostart_pending_free (AutostartPending *pending)
{
    g_weak_ref_clear (&pending->app_ref);
    g_free (pending);
}

/* Nothing was sent and nothing will be, so there is no state to reconcile; the
 * call still owes its callback whatever the callback is holding. */
static void
autostart_pending_discard (AutostartPending *pending)
{
    if (pending->done != NULL)
    {
        const AutostartResult result = { .autostart  = FALSE,
                                         .background = FALSE,
                                         .superseded = TRUE };
        pending->done (&result, pending->user_data);
    }
    autostart_pending_free (pending);
}

static void
autostart_drain_queue (void)
{
    /* Reconciliation runs before the queue is drained and can itself apply, in
     * which case that request is now the one in flight and this one waits for
     * it, still holding the newest intent. */
    if (autostart_inflight != NULL || autostart_queued == NULL)
        return;

    AutostartPending *next = g_steal_pointer (&autostart_queued);
    g_autoptr (OTPClientApplication) app = g_weak_ref_get (&next->app_ref);

    if (app != NULL)
        autostart_start_request (app, next->wanted_autostart, next->generation,
                                 next->done, next->user_data);
    else if (next->done != NULL)
    {
        const AutostartResult none = { .autostart = FALSE, .background = FALSE };
        autostart_deliver (NULL, next->wanted_autostart, next->generation,
                           &none, next->done, next->user_data);
    }

    autostart_pending_free (next);
}

static void
autostart_request_unref (AutostartRequest *req)
{
    if (--req->refs > 0)
        return;

    g_weak_ref_clear (&req->app_ref);
    g_clear_object (&req->bus);
    g_free (req->request_path);
    g_free (req);
}

static void
autostart_request_finish (AutostartRequest      *req,
                          const AutostartResult *result)
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

    /* Cleared before anything is delivered: reconciliation and the callback can
     * both apply, and a request issued from in there belongs in front of the
     * queue, not behind a request that is already over. */
    if (autostart_inflight == req)
        autostart_inflight = NULL;

    g_autoptr (OTPClientApplication) app = g_weak_ref_get (&req->app_ref);
    autostart_deliver (app, req->wanted_autostart, req->generation, result,
                       req->done, req->user_data);

    autostart_request_unref (req);
    autostart_drain_queue ();
}

/* Nothing was answered, so nothing can be claimed. Note that this reports the
 * autostart half as refused even when the request was a removal: a deletion we
 * never heard back about is exactly as unproven as a creation. */
static void
autostart_request_fail (AutostartRequest *req)
{
    const AutostartResult none = { .autostart = FALSE, .background = FALSE };
    autostart_request_finish (req, &none);
}

/* Giving up locally is not the same as the request being over. The portal keeps
 * a Request object alive until it is answered or closed, and an answer that
 * lands after we have stopped listening still changes the desktop: it would
 * silently undo the rollback we are about to do, or apply an intent the next
 * request in the queue has already replaced. That last one is the very race
 * serialising these was meant to remove, so every path that abandons a request
 * has to take it away from the portal as well, not just stop listening to it.
 *
 * Fire and forget. If the object never existed or is already gone the call
 * fails, which is the outcome we wanted anyway, and if an old portal put the
 * request somewhere other than the path we predicted there is nothing better to
 * aim at. */
static void
autostart_close_request (AutostartRequest *req)
{
    g_dbus_connection_call (req->bus, PORTAL_BUS_NAME, req->request_path,
                            PORTAL_REQUEST, "Close", NULL, NULL,
                            G_DBUS_CALL_FLAGS_NO_AUTO_START, -1,
                            NULL, NULL, NULL);
}

static gboolean
on_response_timeout (gpointer user_data)
{
    AutostartRequest *req = user_data;
    req->timeout_id = 0;
    g_warning ("The background portal did not answer within %d seconds",
               PORTAL_RESPONSE_TIMEOUT_SECONDS);

    autostart_close_request (req);
    autostart_request_fail (req);
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
        autostart_request_fail (req);
        return;
    }

    /* Checking the response code alone is not enough. Before x-d-p 1.21.2 a
     * failed autostart write still answered 0, with autostart false in the
     * results, and Debian stable and Ubuntu LTS ship older versions. */
    gboolean got_autostart = FALSE;
    gboolean got_background = FALSE;
    if (results != NULL) {
        g_variant_lookup (results, "autostart", "b", &got_autostart);
        g_variant_lookup (results, "background", "b", &got_background);
    }

    /* The entry is what was asked for whether that was creation or removal, so
     * the autostart half is an agreement check, not the raw bit. */
    const AutostartResult result = {
        .autostart  = (got_autostart == req->wanted_autostart),
        .background = got_background,
        /* The one path where the desktop said what it did. */
        .answered   = TRUE,
    };
    autostart_request_finish (req, &result);
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

        /* The message went out, so the portal may well have a live Request for
         * it even though the reply did not come back, and the next request in
         * the queue is about to start. Only when nothing has arrived yet: a
         * Response that beat the reply means the request is already over. */
        if (!req->finished)
            autostart_close_request (req);

        /* x-d-p only exports the interface when a backend implements it, so an
         * error naming it absent is a desktop that can never do this rather
         * than one that did not manage it this time. The probe in autostart_init
         * asks the same question, but it is asynchronous and the first re-assert
         * of a launch can easily beat it, so the answer is taken from whichever
         * gets one first. Anything vaguer leaves the state alone; see
         * autostart_policy_error_means_no_backend. */
        if (autostart_policy_error_means_no_backend (err))
            backend_state = BACKEND_MISSING;

        autostart_request_fail (req);
    }
    else
    {
        /* The interface took the call, which is the strongest evidence there is
         * that it exists. Recorded whether or not the request is still running,
         * and worth recording in its own right: it lifts a BACKEND_MISSING an
         * earlier error latched, so a portal that has since gained a backend is
         * not written off for the rest of the run. */
        backend_state = BACKEND_AVAILABLE;

        /* The path is predictable and we subscribed to it before calling, which
         * is the whole point of the handle_token. Older portals could hand back
         * something else, in which case move the subscription. */
        const gchar *handle = NULL;
        g_variant_get (reply, "(&o)", &handle);
        if (!req->finished && g_strcmp0 (handle, req->request_path) != 0)
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
        /* x-d-p only exports the interface when a backend implements it, so an
         * error naming it absent is how "the desktop cannot do this" arrives.
         * It is a distinct outcome from the user saying no, which comes much
         * later and as a Response code, and from the probe simply not getting
         * through, which is most of what can go wrong here and is worth nothing
         * as evidence.
         *
         * InvalidArgs counts on this call and on no other: Properties.Get is
         * what GLib answers "No such interface" with, and the interface name
         * and property name are the only arguments it took, both compile-time
         * constants, so there is nothing else here that could be invalid. */
        if (autostart_policy_error_means_no_backend (err)
            || g_error_matches (err, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS))
        {
            g_debug ("No background portal on this session: %s", err->message);
            backend_state = BACKEND_MISSING;
        }
        else
        {
            g_debug ("Could not probe for the background portal: %s", err->message);
        }
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
        /* Says nothing about the portal, only about this moment. Left unknown
         * so the switch stays usable and the retry marker stays alive; a
         * request that really cannot be served will say so itself, with an
         * error that names what is absent. */
        g_debug ("No session bus to probe for the background portal on");
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

gboolean
autostart_entry_may_exist (void)
{
    /* The entry the portal writes lands in the host's config directory, which
     * is not on this side of the sandbox. Nothing to read back. */
    return FALSE;
}

void
autostart_apply (OTPClientApplication *app,
                 gboolean              enable_autostart,
                 AutostartResultFunc   done,
                 gpointer              user_data)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (app));

    const guint generation = ++autostart_generation;

    if (autostart_inflight != NULL)
    {
        /* Whatever was waiting was the newest intent until a moment ago. It
         * never went anywhere, so there is nothing to undo, only a callback to
         * settle. */
        if (autostart_queued != NULL)
            autostart_pending_discard (g_steal_pointer (&autostart_queued));

        AutostartPending *pending = g_new0 (AutostartPending, 1);
        g_weak_ref_init (&pending->app_ref, app);
        pending->wanted_autostart = enable_autostart;
        pending->generation = generation;
        pending->done = done;
        pending->user_data = user_data;
        autostart_queued = pending;
        return;
    }

    autostart_start_request (app, enable_autostart, generation, done, user_data);
}

static void
autostart_start_request (OTPClientApplication *app,
                         gboolean              enable_autostart,
                         guint                 generation,
                         AutostartResultFunc   done,
                         gpointer              user_data)
{
    /* Never reached the portal, so neither half can be claimed. */
    const AutostartResult none = { .autostart = FALSE, .background = FALSE };

    /* Neither failure below is evidence about the portal: the request never got
     * far enough to ask it anything. backend_state is deliberately left where it
     * was, so the answer goes back unanswered, the reconciliation leaves its
     * durable note, and a session bus that was briefly unreachable costs a retry
     * on the next launch rather than the ability to ever retry again. */
    g_autoptr (GError) err = NULL;
    GDBusConnection *bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &err);
    if (bus == NULL)
    {
        g_warning ("Cannot reach the session bus: %s", err->message);
        autostart_deliver (app, enable_autostart, generation, &none, done, user_data);
        autostart_drain_queue ();
        return;
    }

    g_autofree gchar *token = g_strdup_printf ("otpclient_%u", g_random_int ());
    g_autofree gchar *path = build_request_path (bus, token);
    if (path == NULL)
    {
        g_warning ("The session bus connection has no unique name yet");
        g_object_unref (bus);
        autostart_deliver (app, enable_autostart, generation, &none, done, user_data);
        autostart_drain_queue ();
        return;
    }

    AutostartRequest *req = g_new0 (AutostartRequest, 1);
    req->refs = 2;   /* the pending call, and the response side */
    req->bus = bus;  /* transferred */
    req->request_path = g_steal_pointer (&path);
    req->wanted_autostart = enable_autostart;
    req->generation = generation;
    g_weak_ref_init (&req->app_ref, app);
    req->done = done;
    req->user_data = user_data;
    autostart_inflight = req;

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
autostart_ensure_background (OTPClientApplication *app,
                             AutostartResultFunc   done,
                             gpointer              user_data)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (app));

    /* The autostart value has to be restated even though nobody here cares
     * about it: RequestBackground defaults it to FALSE, so omitting it would
     * delete the entry as a side effect of asking to stay alive. */
    autostart_apply (app, otpclient_application_get_autostart (app), done, user_data);
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

gboolean
autostart_entry_may_exist (void)
{
    g_autofree gchar *path = autostart_file_path ();
    return g_file_test (path, G_FILE_TEST_EXISTS);
}

void
autostart_apply (OTPClientApplication *app,
                 gboolean              enable_autostart,
                 AutostartResultFunc   done,
                 gpointer              user_data)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (app));

    /* Kept in step with the portal build even though the work below finishes
     * before this returns, so nothing here can ever be overtaken and nothing
     * needs queueing behind anything. */
    const guint generation = ++autostart_generation;

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

    /* Nothing here can stop a windowless app from running, so the only thing
     * that could have gone wrong is the file. */
    const AutostartResult result = { .autostart = ok, .background = TRUE,
                                     .answered = TRUE };
    autostart_deliver (app, enable_autostart, generation, &result, done, user_data);
}

void
autostart_ensure_background (OTPClientApplication *app,
                             AutostartResultFunc   done,
                             gpointer              user_data)
{
    (void) app;

    /* Nothing kills a windowless app on the host, and writing an autostart file
     * here would turn "minimize to tray" into "start at login" behind the
     * user's back. Answering yes to both is the truth: staying alive needs no
     * grant, and an entry nobody touched is still in whatever state it was. */
    if (done != NULL)
    {
        const AutostartResult result = { .autostart = TRUE, .background = TRUE,
                                         .answered = TRUE };
        done (&result, user_data);
    }
}

#endif /* IS_FLATPAK */

/* Runs for every answer that still stands, whatever asked for it. One request
 * carries both halves and no caller is in a position to act on the half it did
 * not ask about: the Settings toggle sees the background verdict its request
 * also collected, the tray setter sees the autostart verdict, and the argv
 * refresh behind start-minimized has no callback at all. Judging both here is
 * the only arrangement where none of them can drop one.
 *
 * The rule is the same for both keys: nothing can be read back, so a key must
 * never claim more than the desktop actually did. */
static void
autostart_reconcile (OTPClientApplication  *app,
                     gboolean               wanted_autostart,
                     const AutostartResult *result)
{
    const AutostartActions actions =
        autostart_policy_decide (wanted_autostart, result,
                                 autostart_is_supported (),
                                 otpclient_application_get_autostart (app),
                                 otpclient_application_get_minimize_to_tray (app));

    if (actions.write_autostart)
    {
        g_message ("The desktop would not %s the login-time entry",
                   wanted_autostart ? "create" : "remove");
        otpclient_application_set_autostart (app, actions.autostart_value);
        otpclient_application_report_startup_change (app,
            _("The desktop refused to change the login-time launch"));
    }

    if (actions.disable_tray)
    {
        g_message ("Background access is no longer permitted, disabling minimize to tray");
        otpclient_application_set_minimize_to_tray (app, FALSE);
        otpclient_application_report_startup_change (app,
            _("The desktop refused background access, so Minimize to Tray was turned off"));
    }

    if (actions.write_pending)
        otpclient_application_set_startup_reconcile_pending (app, actions.pending_value);

    /* Last of the four, and after the marker on purpose. It issues another
     * request, which bumps the generation and runs a reconciliation of its own;
     * everything above describes this answer rather than that one, and the
     * marker written above is the note that covers the gap until that second
     * answer arrives. On the host build the whole thing happens inside this
     * call, so the ordering is what leaves the later verdict on top. */
    if (actions.retry_removal)
        autostart_apply (app, FALSE, NULL, NULL);
}

void
autostart_reassert (OTPClientApplication *app)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (app));

    /* One request answers for both keys and the reconciliation acts on both, so
     * there is nothing to pass and nothing to wait for. When autostart is off
     * this doubles as the removal of an entry the key does not know about,
     * which is what a stale import or a half-applied earlier run leaves. */
    autostart_apply (app, otpclient_application_get_autostart (app), NULL, NULL);
}
