#ifdef ENABLE_MINIMIZE_TO_TRAY

#include <unistd.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include "tray.h"
#include "tray-menu-model.h"
#include "otpclient-application.h"

#define SNI_OBJECT_PATH    "/StatusNotifierItem"
#define DBUSMENU_OBJECT_PATH "/StatusNotifierMenu"

#define WATCHER_BUS_NAME     "org.kde.StatusNotifierWatcher"
#define WATCHER_OBJECT_PATH  "/StatusNotifierWatcher"

#define MENU_ID_SHOW  1
#define MENU_ID_QUIT  2

/* Whether a StatusNotifierWatcher (and, where it says so, a host behind it) is
 * on the session bus. UNKNOWN covers the startup window before the name watcher
 * has reported in: the UI treats it as "maybe", while the close-request handler
 * keys off `published` and so stays fail-safe either way. */
typedef enum
{
    TRAY_HOST_UNKNOWN = 0,
    TRAY_HOST_AVAILABLE,
    TRAY_HOST_UNAVAILABLE
} TrayHostState;

typedef struct
{
    OTPClientApplication *app;
    GDBusConnection *connection;        /* private bus we published the item on */
    GDBusConnection *watch_connection;  /* shared bus the watcher was spotted on */
    GCancellable *cancellable;          /* covers every async step of one publish */
    guint sni_registration_id;
    guint menu_registration_id;
    guint bus_name_id;
    guint watcher_watch_id;
    guint host_signal_id;
    guint hidden_deadline_id;  /* fail-safe for a started-hidden window */
    gulong close_handler_id;
    gchar *bus_name;
    gchar *activation_token;   /* handed to us by the host, good for one present */
    /* Bumped by every publish and every unpublish. Async replies carry the
     * value they were issued under, so a reply from cycle N is ignored during
     * cycle N+1 instead of resurrecting an item we already tore down. */
    guint publish_generation;
    TrayHostState host;
    gboolean desired;          /* the user's minimize-to-tray preference */
    gboolean publishing;       /* publish in flight, not yet confirmed */
    gboolean published;        /* the watcher accepted our item: a tray icon exists */
    gboolean used_unique_name; /* we registered under :1.x, not the well-known name */
    gboolean holding;          /* a g_application_hold of ours is outstanding */
    gboolean window_hidden;    /* we tucked the window away on close */
} TrayData;

static TrayData *tray_data = NULL;

static void tray_publish   (TrayData *td);
static void tray_unpublish (TrayData *td);

/* The window is on screen, however it got there, so it is neither tucked away
 * nor waiting on a timer that would decide it had been stranded. */
static void
tray_mark_window_shown (TrayData *td)
{
    td->window_hidden = FALSE;
    g_clear_handle_id (&td->hidden_deadline_id, g_source_remove);
}

static void
show_window (TrayData *td)
{
    tray_mark_window_shown (td);

    /* The main window specifically, not the active one: while we are tucked
     * away nothing is active, and once something is, it may well be a dialog. */
    GtkWindow *window = otpclient_application_get_window (td->app);
    if (window != NULL)
    {
        /* Without the host's activation token, Wayland's focus-stealing
         * prevention refuses the raise and the window stays where it is. The
         * token is single use, so consume it. */
        if (td->activation_token != NULL)
        {
            gtk_window_set_startup_id (window, td->activation_token);
            g_clear_pointer (&td->activation_token, g_free);
        }

        /* Through the funnel rather than presenting here: a started-hidden
         * launch still owes the user an unlock prompt, and possibly a
         * what's-new or missing-database dialog. */
        otpclient_application_present_window (td->app);
    }
}

/* How long a tucked-away window waits for a tray icon to come back before it
 * concludes there is nothing left to come back from.
 *
 * Un-hiding the instant the item went away was too eager. Everything that takes
 * a StatusNotifierWatcher off the bus for a second or two, a GNOME Shell
 * restart, an extension reload, plasmashell --replace, went through the same
 * path as a panel that is gone for good, so a blip raised the window unasked
 * and, on a --start-minimized launch, put an unprompted password dialog on
 * screen, possibly mid screen-share. A watcher that comes back republishes and
 * clears this; one that does not still gets the window back. */
#define TRAY_LOST_GRACE_SECONDS 3

static gboolean
on_tray_lost_grace (gpointer user_data)
{
    TrayData *td = user_data;
    td->hidden_deadline_id = 0;

    if (td->window_hidden && !td->published)
    {
        g_message ("The tray item did not come back within %d seconds; showing the window",
                   TRAY_LOST_GRACE_SECONDS);
        show_window (td);
    }

    return G_SOURCE_REMOVE;
}

/* Shares the timer with the started-hidden deadline: both ask the one question
 * of whether a hidden window has been stranded, only ever one of them is
 * outstanding, and a successful publish clears the field either way. */
static void
tray_arm_lost_grace (TrayData *td)
{
    g_clear_handle_id (&td->hidden_deadline_id, g_source_remove);
    td->hidden_deadline_id = g_timeout_add_seconds (TRAY_LOST_GRACE_SECONDS,
                                                    on_tray_lost_grace, td);
}

/* The hold is what lets the app outlive its only window while it sits in the
 * tray. It must track `published` exactly: holding without a visible icon
 * leaves an unreachable process running with a decrypted database in memory. */
static void
tray_sync_hold (TrayData *td)
{
    if (td->published && !td->holding)
    {
        g_application_hold (G_APPLICATION (td->app));
        td->holding = TRUE;
    }
    else if (!td->published && td->holding)
    {
        td->holding = FALSE;
        g_application_release (G_APPLICATION (td->app));
    }
}

/* A property getter that returns NULL without setting the error trips a hard
 * g_assert (error != NULL) inside GDBus's invoke_get_property_in_idle_cb, so a
 * property added to the introspection XML without a matching branch below would
 * abort the process on a plain Get(). Unreachable while the two stay in sync,
 * which is what the introspection test is for, but the cost of being wrong is
 * too high to leave it to a return NULL. */
static GVariant *
unknown_property (GError      **error,
                  const gchar  *property_name)
{
    g_set_error (error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY,
                 "Unknown property: %s", property_name);
    return NULL;
}

/* Neither vtable below checks `sender`, and that is deliberate rather than an
 * oversight. The peer that calls Activate or Event is the tray *host*, and we
 * have no way to learn its bus name: the watcher only hands out item names, a
 * watcher need not be a host (Waybar next to a Plasma-owned watcher is the
 * everyday case), and several hosts may drive the same item. Any allowlist
 * narrow enough to be worth having would break those desktops. The exposure it
 * would buy back is small in any case: a same-user peer that wants this process
 * gone can send it a signal, and one that wants the window up can ask the
 * shell. Nothing here reads a secret out over the bus either; the worst
 * Activate does is put a window back on a screen its owner is sitting at. */

/* --- StatusNotifierItem D-Bus interface --- */

static void
sni_method_call (GDBusConnection       *connection,
                 const gchar           *sender,
                 const gchar           *object_path,
                 const gchar           *interface_name,
                 const gchar           *method_name,
                 GVariant              *parameters,
                 GDBusMethodInvocation *invocation,
                 gpointer               user_data)
{
    (void) connection;
    (void) sender;
    (void) object_path;
    (void) interface_name;

    TrayData *td = user_data;

    if (g_strcmp0 (method_name, "Activate") == 0 ||
        g_strcmp0 (method_name, "SecondaryActivate") == 0)
    {
        show_window (td);
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "ProvideXdgActivationToken") == 0)
    {
        /* Sent immediately before Activate, so just stash it for show_window. */
        const gchar *token;
        g_variant_get (parameters, "(&s)", &token);
        g_free (td->activation_token);
        td->activation_token = g_strdup (token);

        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "ContextMenu") == 0)
    {
        /* Deliberately does nothing but succeed. The host owns the popup, and
         * this is the fallback it uses when it cannot use our dbusmenu: xapp
         * calls it for a secondary click whenever item->menu is NULL, and
         * Waybar when its Version probe fails. Presenting the window from here
         * would turn a right-click into an un-hide. */
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "Scroll") == 0)
    {
        /* Nothing sensible to scroll through, but answering beats UnknownMethod
         * in the host's log on every wheel event over the icon. */
        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else
    {
        g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                               G_DBUS_ERROR_UNKNOWN_METHOD,
                                               "Unknown method: %s", method_name);
    }
}

static GVariant *
sni_get_property (GDBusConnection  *connection,
                  const gchar      *sender,
                  const gchar      *object_path,
                  const gchar      *interface_name,
                  const gchar      *property_name,
                  GError          **error,
                  gpointer          user_data)
{
    (void) connection;
    (void) sender;
    (void) object_path;
    (void) interface_name;

    TrayData *td = user_data;

    if (g_strcmp0 (property_name, "Category") == 0)
        return g_variant_new_string ("ApplicationStatus");
    if (g_strcmp0 (property_name, "Id") == 0)
        return g_variant_new_string ("otpclient");
    /* Hosts show Title as the icon's tooltip. Saying so is worth it on a
     * start-minimized launch, where the database is deliberately left locked
     * and clicking the icon asks for a password. */
    if (g_strcmp0 (property_name, "Title") == 0)
        return g_variant_new_string (otpclient_application_get_app_locked (td->app)
                                     ? _("OTPClient (locked)") : "OTPClient");
    if (g_strcmp0 (property_name, "Status") == 0)
        return g_variant_new_string (td->desired ? "Active" : "Passive");
    if (g_strcmp0 (property_name, "IconName") == 0)
        return g_variant_new_string ("com.github.paolostivanin.OTPClient");
    if (g_strcmp0 (property_name, "IconThemePath") == 0)
        return g_variant_new_string ("");   /* our icon comes from the theme */
    /* Never "/": xapp treats that as a valid menu, builds a dead one, and the
     * non-NULL menu then suppresses its own ContextMenu fallback. */
    if (g_strcmp0 (property_name, "Menu") == 0)
        return g_variant_new_object_path (DBUSMENU_OBJECT_PATH);
    if (g_strcmp0 (property_name, "ItemIsMenu") == 0)
        return g_variant_new_boolean (FALSE);
    if (g_strcmp0 (property_name, "WindowId") == 0)
        return g_variant_new_int32 (0);     /* no X11 window to point at */

    return unknown_property (error, property_name);
}

static const GDBusInterfaceVTable sni_vtable = {
    .method_call = sni_method_call,
    .get_property = sni_get_property,
    .set_property = NULL,
};

/* --- DBusMenu D-Bus interface --- */

/* The menu itself. The model in tray-menu-model.c turns this into every reply
 * on the interface; the labels stay here so they can go through _() while the
 * model stays free of GTK, gettext and the application object, and so the
 * tests can drive it with their own fixtures. Built on first use because
 * gettext needs the locale to be set up. */
static const TrayMenuEntry *
menu_entries (gsize *n_items)
{
    static TrayMenuEntry entries[2];
    static gsize initialised = 0;

    if (g_once_init_enter (&initialised))
    {
        entries[0] = (TrayMenuEntry) { MENU_ID_SHOW, _("Show OTPClient"), TRAY_MENU_ACTION_SHOW };
        entries[1] = (TrayMenuEntry) { MENU_ID_QUIT, _("Quit"),           TRAY_MENU_ACTION_QUIT };
        g_once_init_leave (&initialised, 1);
    }

    *n_items = G_N_ELEMENTS (entries);
    return entries;
}

static gboolean
quit_in_idle (gpointer user_data)
{
    g_application_quit (G_APPLICATION (user_data));
    return G_SOURCE_REMOVE;
}

/* Quitting is deferred to an idle so the method reply is on the wire first:
 * libdbusmenu's Event call is not annotated NoReply and waits a second for it,
 * so tearing the process down inline would stall the panel. */
static void
tray_perform_action (TrayData      *td,
                     TrayMenuAction action)
{
    switch (action)
    {
        case TRAY_MENU_ACTION_SHOW:
            show_window (td);
            break;
        case TRAY_MENU_ACTION_QUIT:
            g_idle_add (quit_in_idle, td->app);
            break;
        case TRAY_MENU_ACTION_NONE:
        default:
            break;
    }
}

static void
dbusmenu_method_call (GDBusConnection       *connection,
                      const gchar           *sender,
                      const gchar           *object_path,
                      const gchar           *interface_name,
                      const gchar           *method_name,
                      GVariant              *parameters,
                      GDBusMethodInvocation *invocation,
                      gpointer               user_data)
{
    (void) connection;
    (void) sender;
    (void) object_path;
    (void) interface_name;

    TrayData *td = user_data;

    gsize n_items;
    const TrayMenuEntry *items = menu_entries (&n_items);

    if (g_strcmp0 (method_name, "GetLayout") == 0)
    {
        gint32 parent_id, depth;
        g_variant_get (parameters, "(ii@as)", &parent_id, &depth, NULL);

        gboolean found = FALSE;
        GVariant *reply = tray_menu_model_layout (items, n_items, parent_id, depth, &found);
        if (!found)
        {
            g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                   G_DBUS_ERROR_INVALID_ARGS,
                                                   "No menu item %d", parent_id);
            return;
        }

        g_dbus_method_invocation_return_value (invocation, reply);
    }
    else if (g_strcmp0 (method_name, "GetGroupProperties") == 0)
    {
        g_autoptr (GVariant) ids = g_variant_get_child_value (parameters, 0);
        g_dbus_method_invocation_return_value (invocation,
                                               tray_menu_model_group_properties (items, n_items, ids));
    }
    else if (g_strcmp0 (method_name, "GetProperty") == 0)
    {
        gint32 id;
        const gchar *name;
        g_variant_get (parameters, "(i&s)", &id, &name);

        g_autoptr (GVariant) value = tray_menu_model_property (items, n_items, id, name);
        if (value == NULL)
        {
            g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                                   G_DBUS_ERROR_INVALID_ARGS,
                                                   "No property '%s' on menu item %d",
                                                   name, id);
            return;
        }

        g_dbus_method_invocation_return_value (invocation, g_variant_new ("(v)", value));
    }
    else if (g_strcmp0 (method_name, "Event") == 0)
    {
        gint32 id;
        const gchar *event_id;
        /* "&s" borrows from `parameters`; plain "s" would hand back a dup that
         * nothing here frees. */
        g_variant_get (parameters, "(i&s@vu)", &id, &event_id, NULL, NULL);

        tray_perform_action (td, tray_menu_model_action_for_event (items, n_items, id, event_id));

        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "EventGroup") == 0)
    {
        g_autoptr (GVariant) events = g_variant_get_child_value (parameters, 0);
        g_autoptr (GArray) actions = g_array_new (FALSE, FALSE, sizeof (TrayMenuAction));

        GVariant *reply = tray_menu_model_event_group (items, n_items, events, actions);

        for (guint i = 0; i < actions->len; i++)
            tray_perform_action (td, g_array_index (actions, TrayMenuAction, i));

        g_dbus_method_invocation_return_value (invocation, reply);
    }
    else if (g_strcmp0 (method_name, "AboutToShow") == 0)
    {
        g_dbus_method_invocation_return_value (invocation,
                                               g_variant_new ("(b)", FALSE));
    }
    else if (g_strcmp0 (method_name, "AboutToShowGroup") == 0)
    {
        g_autoptr (GVariant) ids = g_variant_get_child_value (parameters, 0);
        g_dbus_method_invocation_return_value (invocation,
                                               tray_menu_model_about_to_show_group (items, n_items, ids));
    }
    else
    {
        g_dbus_method_invocation_return_error (invocation, G_DBUS_ERROR,
                                               G_DBUS_ERROR_UNKNOWN_METHOD,
                                               "Unknown method: %s", method_name);
    }
}

static GVariant *
dbusmenu_get_property (GDBusConnection  *connection,
                       const gchar      *sender,
                       const gchar      *object_path,
                       const gchar      *interface_name,
                       const gchar      *property_name,
                       GError          **error,
                       gpointer          user_data)
{
    (void) connection;
    (void) sender;
    (void) object_path;
    (void) interface_name;
    (void) user_data;

    /* Version 3 is what routes clients to the *Group methods. Downgrading to 2
     * would make the plain Event/AboutToShow pair live again and is a genuine
     * one-line alternative, but it is a silent capability downgrade that the
     * next reader would "fix" back to 3 and re-break the menu. */
    if (g_strcmp0 (property_name, "Version") == 0)
        return g_variant_new_uint32 (3);
    if (g_strcmp0 (property_name, "TextDirection") == 0)
        return g_variant_new_string (gtk_widget_get_default_direction () == GTK_TEXT_DIR_RTL
                                     ? "rtl" : "ltr");
    if (g_strcmp0 (property_name, "Status") == 0)
        return g_variant_new_string ("normal");

    return unknown_property (error, property_name);
}

static const GDBusInterfaceVTable dbusmenu_vtable = {
    .method_call = dbusmenu_method_call,
    .get_property = dbusmenu_get_property,
    .set_property = NULL,
};

/* --- Window close-request handler --- */

static gboolean
on_close_request (GtkWindow *window,
                  gpointer   user_data)
{
    TrayData *td = user_data;

    /* Only swallow the close when there is an icon to restore the window from.
     * Without a live tray item the user would be left with an invisible,
     * unquittable process, so fall through to the normal close instead. */
    if (td->published && otpclient_application_get_minimize_to_tray (td->app))
    {
        gtk_widget_set_visible (GTK_WIDGET (window), FALSE);
        td->window_hidden = TRUE;
        return TRUE;
    }

    return FALSE;
}

/* --- Publishing the StatusNotifierItem --- */

/* The item lives on a private connection to the session bus rather than on the
 * shared one g_bus_get hands out, and that is what makes a tray icon removable
 * at all.
 *
 * org.kde.StatusNotifierWatcher has RegisterStatusNotifierItem and no
 * unregister counterpart, on any implementation: watchers drop an item when the
 * bus name it was registered under loses its owner, and nothing else. Releasing
 * a well-known name does that. The sandboxed path never gets a well-known name
 * (see on_name_lost) and registers the connection's unique name instead, which
 * lives exactly as long as the connection does. On the shared bus that means
 * the process, so turning minimize-to-tray off inside Flatpak would leave a
 * dead icon in the panel until the app exits. A connection of our own can be
 * closed, which drops the unique name and the well-known one together, so one
 * teardown works on both paths. */

/* Every async callback below re-reads the `tray_data` singleton instead of
 * trusting user_data: cleanup NULLs it, so this doubles as a liveness check on
 * the pointer the call was issued with. What user_data carries instead is the
 * publish generation the call went out under, because a reply belonging to a
 * cycle that has already been torn down must not be acted on. */
static gboolean
publish_is_current (gpointer user_data)
{
    return tray_data != NULL
           && tray_data->publishing
           && tray_data->publish_generation == GPOINTER_TO_UINT (user_data);
}

static void
on_item_registered (GObject      *source,
                    GAsyncResult *res,
                    gpointer      user_data)
{
    GError *err = NULL;
    g_autoptr (GVariant) reply =
        g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &err);

    /* Acting on a stale reply would resurrect an item we already tore down,
     * hold included. */
    if (!publish_is_current (user_data))
    {
        g_clear_error (&err);
        return;
    }

    if (reply == NULL)
    {
        g_warning ("StatusNotifierWatcher refused our tray item: %s", err->message);
        g_clear_error (&err);
        tray_data->host = TRAY_HOST_UNAVAILABLE;
        tray_unpublish (tray_data);
        return;
    }

    tray_data->publishing = FALSE;
    tray_data->published = TRUE;
    tray_sync_hold (tray_data);

    /* There is an icon now, so the started-hidden fail-safe has done its job. */
    g_clear_handle_id (&tray_data->hidden_deadline_id, g_source_remove);
}

/* Ask the watcher to adopt the item, and listen to the answer: whether an icon
 * actually exists decides whether closing the window is allowed to hide it.
 * `service` is the bus name the item can be reached at, either our well-known
 * name or, where the bus refused to hand that out, the unique one. */
static void
tray_register_with_watcher (TrayData    *td,
                            const gchar *service)
{
    g_dbus_connection_call (td->connection,
                            WATCHER_BUS_NAME,
                            WATCHER_OBJECT_PATH,
                            "org.kde.StatusNotifierWatcher",
                            "RegisterStatusNotifierItem",
                            g_variant_new ("(s)", service),
                            NULL,
                            G_DBUS_CALL_FLAGS_NONE,
                            -1, td->cancellable, on_item_registered,
                            GUINT_TO_POINTER (td->publish_generation));
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
    (void) connection;
    (void) user_data;

    /* A NameAcquired that arrives after the fallback already registered us
     * would put a second item in the tray. */
    if (tray_data == NULL || !tray_data->publishing || tray_data->used_unique_name)
        return;

    tray_register_with_watcher (tray_data, name);
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
    (void) connection;
    (void) name;
    (void) user_data;

    if (tray_data == NULL)
        return;

    /* The objects are exported before the name is ever requested, so at this
     * point the item is reachable and only the name is missing. Sandboxes are
     * the usual reason: xdg-dbus-proxy answers RequestName with ServiceUnknown
     * unless the Flatpak manifest grants --own-name, and every sandboxed app is
     * pid 2, so the pid-derived name collides between apps anyway. The watcher
     * does not need a well-known name, so register the unique one instead,
     * which is what Qt's tray does (QDBusMenuConnection passes baseService()). */
    if (tray_data->publishing && !tray_data->published &&
        !tray_data->used_unique_name && tray_data->connection != NULL)
    {
        const gchar *unique = g_dbus_connection_get_unique_name (tray_data->connection);
        if (unique != NULL)
        {
            tray_data->used_unique_name = TRUE;
            g_info ("Could not own %s, registering the tray item as %s instead",
                    tray_data->bus_name, unique);
            tray_register_with_watcher (tray_data, unique);
            return;
        }
    }

    g_info ("Lost bus name for StatusNotifierItem");
    tray_unpublish (tray_data);
}

static gboolean
tray_register_objects (TrayData        *td,
                       GDBusConnection *connection)
{
    GError *err = NULL;

    g_autoptr (GDBusNodeInfo) sni_info =
        g_dbus_node_info_new_for_xml (tray_menu_model_sni_introspection_xml, &err);
    if (err != NULL)
    {
        g_warning ("Failed to parse SNI introspection: %s", err->message);
        g_clear_error (&err);
        return FALSE;
    }

    td->sni_registration_id =
        g_dbus_connection_register_object (connection,
                                           SNI_OBJECT_PATH,
                                           sni_info->interfaces[0],
                                           &sni_vtable,
                                           td, NULL, &err);
    if (err != NULL)
    {
        g_warning ("Failed to register SNI object: %s", err->message);
        g_clear_error (&err);
        return FALSE;
    }

    g_autoptr (GDBusNodeInfo) menu_info =
        g_dbus_node_info_new_for_xml (tray_menu_model_dbusmenu_introspection_xml, &err);
    if (err != NULL)
    {
        g_warning ("Failed to parse dbusmenu introspection: %s", err->message);
        g_clear_error (&err);
        return FALSE;
    }

    td->menu_registration_id =
        g_dbus_connection_register_object (connection,
                                           DBUSMENU_OBJECT_PATH,
                                           menu_info->interfaces[0],
                                           &dbusmenu_vtable,
                                           td, NULL, &err);
    if (err != NULL)
    {
        g_warning ("Failed to register dbusmenu object: %s", err->message);
        g_clear_error (&err);
        return FALSE;
    }

    return TRUE;
}

static void
on_private_bus_ready (GObject      *source,
                      GAsyncResult *res,
                      gpointer      user_data)
{
    (void) source;

    GError *err = NULL;
    GDBusConnection *connection = g_dbus_connection_new_for_address_finish (res, &err);

    if (!publish_is_current (user_data))
    {
        /* Cancelled or superseded. Close it rather than leaving a nameless
         * connection of ours parked on the bus for the rest of the session. */
        if (connection != NULL)
        {
            g_dbus_connection_close (connection, NULL, NULL, NULL);
            g_object_unref (connection);
        }
        g_clear_error (&err);
        return;
    }

    if (connection == NULL)
    {
        g_warning ("Could not open a bus connection for the tray item: %s", err->message);
        g_clear_error (&err);
        tray_unpublish (tray_data);
        return;
    }

    /* Nothing about a tray icon is worth taking the process down for, and this
     * connection is closed on every unpublish by design. */
    g_dbus_connection_set_exit_on_close (connection, FALSE);
    tray_data->connection = connection;

    /* Export before asking for the name, the order GLib itself uses for
     * g_bus_own_name, so that the fallback in on_name_lost has something to
     * point the watcher at. */
    if (!tray_register_objects (tray_data, connection))
    {
        tray_unpublish (tray_data);
        return;
    }

    /* DO_NOT_QUEUE: a pid-derived name is not worth waiting in line for, and
     * queueing is what would deliver a late NameAcquired on top of a fallback
     * registration. Two sandboxed apps both at pid 2 now each get an icon. */
    tray_data->bus_name_id =
        g_bus_own_name_on_connection (connection,
                                      tray_data->bus_name,
                                      G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
                                      on_name_acquired,
                                      on_name_lost,
                                      tray_data,
                                      NULL);
}

static void
tray_publish (TrayData *td)
{
    if (td->publishing || td->published)
        return;
    if (!td->desired || td->host != TRAY_HOST_AVAILABLE)
        return;

    /* Reads DBUS_SESSION_BUS_ADDRESS, no I/O, and a watcher was just seen on
     * that very bus, so this only fails in ways worth a warning. */
    GError *err = NULL;
    g_autofree gchar *address =
        g_dbus_address_get_for_bus_sync (G_BUS_TYPE_SESSION, NULL, &err);
    if (address == NULL)
    {
        g_warning ("No session bus address for the tray item: %s", err->message);
        g_clear_error (&err);
        td->host = TRAY_HOST_UNAVAILABLE;
        tray_unpublish (td);
        return;
    }

    td->publishing = TRUE;
    td->publish_generation++;
    td->cancellable = g_cancellable_new ();

    g_dbus_connection_new_for_address (address,
                                       G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
                                       G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
                                       NULL,
                                       td->cancellable,
                                       on_private_bus_ready,
                                       GUINT_TO_POINTER (td->publish_generation));
}

static void
tray_unpublish (TrayData *td)
{
    /* Whatever is still in flight belongs to a cycle that is over now. */
    td->publish_generation++;
    if (td->cancellable != NULL)
    {
        g_cancellable_cancel (td->cancellable);
        g_clear_object (&td->cancellable);
    }

    /* Release the name before withdrawing the objects. The other order leaves a
     * window in which the name still resolves and every call to it answers
     * UnknownObject. */
    if (td->bus_name_id != 0)
    {
        g_bus_unown_name (td->bus_name_id);
        td->bus_name_id = 0;
    }

    if (td->connection != NULL)
    {
        if (td->sni_registration_id != 0)
        {
            g_dbus_connection_unregister_object (td->connection, td->sni_registration_id);
            td->sni_registration_id = 0;
        }
        if (td->menu_registration_id != 0)
        {
            g_dbus_connection_unregister_object (td->connection, td->menu_registration_id);
            td->menu_registration_id = 0;
        }

        /* Closing is the part that actually removes the icon, for the reason at
         * the top of this section. Flush first: close makes no promise about
         * the outgoing queue, and a NewStatus the caller emitted a moment ago
         * is still in it. Safe to do synchronously, every caller is on the main
         * context and the write is to a local socket. */
        g_dbus_connection_flush_sync (td->connection, NULL, NULL);
        g_dbus_connection_close (td->connection, NULL, NULL, NULL);
        g_clear_object (&td->connection);
    }

    td->publishing = FALSE;
    td->published = FALSE;
    td->used_unique_name = FALSE;
    tray_sync_hold (td);

    /* The panel can go away (extension toggled off, shell restarted) while the
     * window is tucked into the tray. Bring it back rather than stranding it,
     * but give it a moment first: see the grace period below. */
    if (td->window_hidden)
        tray_arm_lost_grace (td);
}

/* --- StatusNotifierWatcher detection --- */

static void
tray_set_host_available (TrayData *td,
                         gboolean  available)
{
    TrayHostState state = available ? TRAY_HOST_AVAILABLE : TRAY_HOST_UNAVAILABLE;

    if (td->host == state)
        return;

    td->host = state;

    if (available)
        tray_publish (td);
    else
        tray_unpublish (td);
}

static void
on_host_registered (GDBusConnection *connection,
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
    (void) parameters;

    tray_set_host_available (user_data, TRUE);
}

static void
on_host_property_read (GObject      *source,
                       GAsyncResult *res,
                       gpointer      user_data)
{
    (void) user_data;

    GError *err = NULL;
    g_autoptr (GVariant) reply =
        g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &err);

    /* watch_connection is cleared when the watcher goes away, so a NULL here
     * means the watcher we were asking about is already gone. */
    if (tray_data == NULL || tray_data->watch_connection == NULL)
    {
        g_clear_error (&err);
        return;
    }

    gboolean host_registered = TRUE;

    if (reply == NULL)
    {
        /* Not every watcher implements the property. Its presence on the bus is
         * a good enough signal on its own, so don't refuse the feature over it. */
        g_debug ("Could not read IsStatusNotifierHostRegistered: %s", err->message);
        g_clear_error (&err);
    }
    else
    {
        g_autoptr (GVariant) value = NULL;
        g_variant_get (reply, "(v)", &value);
        if (g_variant_is_of_type (value, G_VARIANT_TYPE_BOOLEAN))
            host_registered = g_variant_get_boolean (value);
    }

    tray_set_host_available (tray_data, host_registered);
}

static void
on_watcher_appeared (GDBusConnection *connection,
                     const gchar     *name,
                     const gchar     *name_owner,
                     gpointer         user_data)
{
    (void) name;

    TrayData *td = user_data;
    g_set_object (&td->watch_connection, connection);

    /* A watcher can be up before any host has registered with it, so keep
     * listening after the initial property read. */
    td->host_signal_id =
        g_dbus_connection_signal_subscribe (connection,
                                            name_owner,
                                            "org.kde.StatusNotifierWatcher",
                                            "StatusNotifierHostRegistered",
                                            WATCHER_OBJECT_PATH,
                                            NULL,
                                            G_DBUS_SIGNAL_FLAGS_NONE,
                                            on_host_registered,
                                            td,
                                            NULL);

    g_dbus_connection_call (connection,
                            WATCHER_BUS_NAME,
                            WATCHER_OBJECT_PATH,
                            "org.freedesktop.DBus.Properties",
                            "Get",
                            g_variant_new ("(ss)", "org.kde.StatusNotifierWatcher",
                                                   "IsStatusNotifierHostRegistered"),
                            G_VARIANT_TYPE ("(v)"),
                            G_DBUS_CALL_FLAGS_NONE,
                            -1, NULL, on_host_property_read, NULL);
}

static void
on_watcher_vanished (GDBusConnection *connection,
                     const gchar     *name,
                     gpointer         user_data)
{
    (void) connection;
    (void) name;

    TrayData *td = user_data;

    if (td->host_signal_id != 0 && td->watch_connection != NULL)
    {
        g_dbus_connection_signal_unsubscribe (td->watch_connection, td->host_signal_id);
        td->host_signal_id = 0;
    }
    g_clear_object (&td->watch_connection);

    tray_set_host_available (td, FALSE);
}

/* --- Public API --- */

void
otpclient_tray_init (OTPClientApplication *app)
{
    if (tray_data != NULL)
        return;

    tray_data = g_new0 (TrayData, 1);
    tray_data->app = app;
    tray_data->host = TRAY_HOST_UNKNOWN;
    tray_data->desired = otpclient_application_get_minimize_to_tray (app);

    tray_data->bus_name = g_strdup_printf ("org.kde.StatusNotifierItem-%d-1",
                                            getpid ());

    /* Same accessor as the disconnect in cleanup, and as show_window: the three
     * have to agree on which window they mean, and the active one is not it. */
    GtkWindow *window = otpclient_application_get_window (app);
    if (window != NULL)
    {
        tray_data->close_handler_id =
            g_signal_connect (window, "close-request",
                              G_CALLBACK (on_close_request), tray_data);
    }

    /* The item is published lazily, once a watcher is known to be there and the
     * user has actually asked for minimize-to-tray. Registering unconditionally
     * would park a Passive item in the tray overflow of every desktop that
     * shows them, for a feature the user never enabled. */
    tray_data->watcher_watch_id =
        g_bus_watch_name (G_BUS_TYPE_SESSION,
                          WATCHER_BUS_NAME,
                          G_BUS_NAME_WATCHER_FLAGS_NONE,
                          on_watcher_appeared,
                          on_watcher_vanished,
                          tray_data,
                          NULL);
}

/* Seconds to wait for an icon before giving up and showing the window. Long
 * enough for a cold Plasma login, short enough that the user does not conclude
 * the app failed to start. */
#define START_HIDDEN_DEADLINE_SECONDS 10

static gboolean
on_start_hidden_deadline (gpointer user_data)
{
    TrayData *td = user_data;
    td->hidden_deadline_id = 0;

    if (td->window_hidden)
    {
        g_message ("No tray icon appeared within %d seconds; showing the window",
                   START_HIDDEN_DEADLINE_SECONDS);
        show_window (td);
    }

    return G_SOURCE_REMOVE;
}

void
otpclient_tray_begin_hidden (OTPClientApplication *app)
{
    (void) app;

    if (tray_data == NULL || tray_data->window_hidden)
        return;

    tray_data->window_hidden = TRUE;

    /* Nearly every way this can fail already routes through tray_unpublish,
     * which ends by showing a hidden window: the watcher name having no owner
     * (g_bus_watch_name fires vanished at watch time in that case),
     * IsStatusNotifierHostRegistered coming back false, and the watcher
     * refusing our item. Those need no timer and self-heal immediately.
     *
     * What is left is a watcher that owns the name but never answers the
     * property read and never emits StatusNotifierHostRegistered: host stays
     * UNKNOWN, so nothing publishes and nothing unpublishes, and the window
     * would sit hidden forever. This also covers the genuinely undetectable
     * case where the watcher accepts the item and the panel never draws it. */
    tray_data->hidden_deadline_id =
        g_timeout_add_seconds (START_HIDDEN_DEADLINE_SECONDS,
                               on_start_hidden_deadline, tray_data);
}

void
otpclient_tray_enable (OTPClientApplication *app)
{
    if (tray_data == NULL)
    {
        otpclient_tray_init (app);
        return;
    }

    if (tray_data->desired)
        return;

    tray_data->desired = TRUE;

    if (tray_data->published && tray_data->connection != NULL)
    {
        g_dbus_connection_emit_signal (tray_data->connection,
                                       NULL,
                                       SNI_OBJECT_PATH,
                                       "org.kde.StatusNotifierItem",
                                       "NewStatus",
                                       g_variant_new ("(s)", "Active"),
                                       NULL);
    }
    else
    {
        tray_publish (tray_data);
    }
}

void
otpclient_tray_disable (OTPClientApplication *app)
{
    (void) app;

    if (tray_data == NULL || !tray_data->desired)
        return;

    tray_data->desired = FALSE;

    /* Tell any host that cached the item before tearing it down, so it doesn't
     * hold on to a stale Active entry. tray_unpublish flushes this out before
     * it closes the connection, otherwise the signal would still be sitting in
     * the outgoing queue when the socket goes away. */
    if (tray_data->published && tray_data->connection != NULL)
    {
        g_dbus_connection_emit_signal (tray_data->connection,
                                       NULL,
                                       SNI_OBJECT_PATH,
                                       "org.kde.StatusNotifierItem",
                                       "NewStatus",
                                       g_variant_new ("(s)", "Passive"),
                                       NULL);
    }

    tray_unpublish (tray_data);

    /* The user turned the feature off, so there is nothing left to wait for and
     * no reason to make them wait for it. The grace period is for a panel that
     * might still come back. */
    if (tray_data->window_hidden)
        show_window (tray_data);
}

void
otpclient_tray_notify_window_shown (OTPClientApplication *app)
{
    (void) app;

    if (tray_data == NULL)
        return;

    tray_mark_window_shown (tray_data);
}

void
otpclient_tray_notify_locked_changed (OTPClientApplication *app)
{
    (void) app;

    if (tray_data == NULL || !tray_data->published || tray_data->connection == NULL)
        return;

    /* NewTitle carries no payload; the host re-reads the property. */
    g_dbus_connection_emit_signal (tray_data->connection,
                                   NULL,
                                   SNI_OBJECT_PATH,
                                   "org.kde.StatusNotifierItem",
                                   "NewTitle",
                                   NULL,
                                   NULL);
}

gboolean
otpclient_tray_is_available (void)
{
    return tray_data != NULL && tray_data->host != TRAY_HOST_UNAVAILABLE;
}

gboolean
otpclient_tray_watcher_present_sync (void)
{
    /* Whatever the name watcher has managed to learn already is both cheaper
     * and stronger than a fresh probe: AVAILABLE means a host confirmed it,
     * UNAVAILABLE that the name has no owner. Only UNKNOWN is worth a call. */
    if (tray_data != NULL && tray_data->host != TRAY_HOST_UNKNOWN)
        return tray_data->host == TRAY_HOST_AVAILABLE;

    g_autoptr (GError) err = NULL;
    g_autoptr (GDBusConnection) bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &err);
    if (bus == NULL)
    {
        g_debug ("No session bus to look for a tray watcher on: %s", err->message);
        return FALSE;
    }

    g_autoptr (GVariant) reply =
        g_dbus_connection_call_sync (bus,
                                     "org.freedesktop.DBus",
                                     "/org/freedesktop/DBus",
                                     "org.freedesktop.DBus",
                                     "NameHasOwner",
                                     g_variant_new ("(s)", WATCHER_BUS_NAME),
                                     G_VARIANT_TYPE ("(b)"),
                                     G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                     /* The bus daemon itself, so a short wait
                                      * is generous and a launch is never held
                                      * up by a busy session. */
                                     2000, NULL, &err);
    if (reply == NULL)
    {
        g_debug ("Could not ask the bus for a tray watcher: %s", err->message);
        return FALSE;
    }

    gboolean has_owner = FALSE;
    g_variant_get (reply, "(b)", &has_owner);

    return has_owner;
}

void
otpclient_tray_cleanup (OTPClientApplication *app)
{
    if (tray_data == NULL)
        return;

    if (tray_data->close_handler_id != 0)
    {
        /* NULL here means GTK already destroyed the window and took the handler
         * with it. Anything else would be the wrong window: disconnecting a
         * handler id it never had is a critical.
         *
         * A non-NULL window is not proof on its own, either. The application's
         * weak pointer nulls in finalize, but g_signal_handlers_destroy runs in
         * dispose, so between the two the window is still addressable and the
         * id is already dead. Ask before disconnecting. */
        GtkWindow *window = otpclient_application_get_window (app);
        if (window != NULL
            && g_signal_handler_is_connected (window, tray_data->close_handler_id))
            g_clear_signal_handler (&tray_data->close_handler_id, window);

        tray_data->close_handler_id = 0;
    }

    if (tray_data->host_signal_id != 0 && tray_data->watch_connection != NULL)
        g_dbus_connection_signal_unsubscribe (tray_data->watch_connection,
                                              tray_data->host_signal_id);
    g_clear_object (&tray_data->watch_connection);

    if (tray_data->watcher_watch_id != 0)
        g_bus_unwatch_name (tray_data->watcher_watch_id);

    g_clear_handle_id (&tray_data->hidden_deadline_id, g_source_remove);

    /* Clears the hold too, so the teardown doesn't leave the app held. */
    tray_data->window_hidden = FALSE;
    tray_unpublish (tray_data);

    g_free (tray_data->bus_name);
    g_free (tray_data->activation_token);
    g_free (tray_data);
    tray_data = NULL;
}

#endif
