#ifdef ENABLE_MINIMIZE_TO_TRAY

#include <unistd.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include "tray.h"
#include "otpclient-application.h"

#define SNI_OBJECT_PATH    "/StatusNotifierItem"
#define DBUSMENU_OBJECT_PATH "/StatusNotifierMenu"

#define WATCHER_BUS_NAME     "org.kde.StatusNotifierWatcher"
#define WATCHER_OBJECT_PATH  "/StatusNotifierWatcher"

#define MENU_ID_SHOW  1
#define MENU_ID_QUIT  2

static const gchar sni_introspection_xml[] =
    "<node>"
    "  <interface name='org.kde.StatusNotifierItem'>"
    "    <property name='Category' type='s' access='read'/>"
    "    <property name='Id' type='s' access='read'/>"
    "    <property name='Title' type='s' access='read'/>"
    "    <property name='Status' type='s' access='read'/>"
    "    <property name='IconName' type='s' access='read'/>"
    "    <property name='Menu' type='o' access='read'/>"
    "    <property name='ItemIsMenu' type='b' access='read'/>"
    "    <method name='Activate'>"
    "      <arg type='i' name='x' direction='in'/>"
    "      <arg type='i' name='y' direction='in'/>"
    "    </method>"
    "    <method name='SecondaryActivate'>"
    "      <arg type='i' name='x' direction='in'/>"
    "      <arg type='i' name='y' direction='in'/>"
    "    </method>"
    "    <signal name='NewStatus'>"
    "      <arg type='s' name='status'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

static const gchar dbusmenu_introspection_xml[] =
    "<node>"
    "  <interface name='com.canonical.dbusmenu'>"
    "    <property name='Version' type='u' access='read'/>"
    "    <property name='TextDirection' type='s' access='read'/>"
    "    <property name='Status' type='s' access='read'/>"
    "    <method name='GetLayout'>"
    "      <arg type='i' name='parentId' direction='in'/>"
    "      <arg type='i' name='recursionDepth' direction='in'/>"
    "      <arg type='as' name='propertyNames' direction='in'/>"
    "      <arg type='u' name='revision' direction='out'/>"
    "      <arg type='(ia{sv}av)' name='layout' direction='out'/>"
    "    </method>"
    "    <method name='Event'>"
    "      <arg type='i' name='id' direction='in'/>"
    "      <arg type='s' name='eventId' direction='in'/>"
    "      <arg type='v' name='data' direction='in'/>"
    "      <arg type='u' name='timestamp' direction='in'/>"
    "    </method>"
    "    <method name='AboutToShow'>"
    "      <arg type='i' name='id' direction='in'/>"
    "      <arg type='b' name='needUpdate' direction='out'/>"
    "    </method>"
    "    <signal name='LayoutUpdated'>"
    "      <arg type='u' name='revision'/>"
    "      <arg type='i' name='parent'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

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
    GDBusConnection *connection;        /* bus we published the item on */
    GDBusConnection *watch_connection;  /* bus the watcher was spotted on */
    guint sni_registration_id;
    guint menu_registration_id;
    guint bus_name_id;
    guint watcher_watch_id;
    guint host_signal_id;
    gulong close_handler_id;
    gchar *bus_name;
    TrayHostState host;
    gboolean desired;          /* the user's minimize-to-tray preference */
    gboolean publishing;       /* bus name request in flight, not yet confirmed */
    gboolean published;        /* the watcher accepted our item: a tray icon exists */
    gboolean used_unique_name; /* we registered under :1.x, not the well-known name */
    gboolean holding;          /* a g_application_hold of ours is outstanding */
    gboolean window_hidden;    /* we tucked the window away on close */
} TrayData;

static TrayData *tray_data = NULL;

static void tray_publish   (TrayData *td);
static void tray_unpublish (TrayData *td);

static void
show_window (TrayData *td)
{
    td->window_hidden = FALSE;

    GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (td->app));
    if (window != NULL)
    {
        gtk_widget_set_visible (GTK_WIDGET (window), TRUE);
        gtk_window_present (window);
    }
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
    (void) parameters;

    TrayData *td = user_data;

    if (g_strcmp0 (method_name, "Activate") == 0 ||
        g_strcmp0 (method_name, "SecondaryActivate") == 0)
    {
        show_window (td);
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
    (void) error;

    TrayData *td = user_data;

    if (g_strcmp0 (property_name, "Category") == 0)
        return g_variant_new_string ("ApplicationStatus");
    if (g_strcmp0 (property_name, "Id") == 0)
        return g_variant_new_string ("otpclient");
    if (g_strcmp0 (property_name, "Title") == 0)
        return g_variant_new_string ("OTPClient");
    if (g_strcmp0 (property_name, "Status") == 0)
        return g_variant_new_string (td->desired ? "Active" : "Passive");
    if (g_strcmp0 (property_name, "IconName") == 0)
        return g_variant_new_string ("com.github.paolostivanin.OTPClient");
    if (g_strcmp0 (property_name, "Menu") == 0)
        return g_variant_new_object_path (DBUSMENU_OBJECT_PATH);
    if (g_strcmp0 (property_name, "ItemIsMenu") == 0)
        return g_variant_new_boolean (FALSE);

    return NULL;
}

static const GDBusInterfaceVTable sni_vtable = {
    .method_call = sni_method_call,
    .get_property = sni_get_property,
    .set_property = NULL,
};

/* --- DBusMenu D-Bus interface --- */

static GVariant *
build_menu_item (gint32       id,
                 const gchar *label,
                 gboolean     is_root)
{
    GVariantBuilder props;
    g_variant_builder_init (&props, G_VARIANT_TYPE ("a{sv}"));

    if (!is_root)
    {
        g_variant_builder_add (&props, "{sv}", "label",
                               g_variant_new_string (label));
        g_variant_builder_add (&props, "{sv}", "enabled",
                               g_variant_new_boolean (TRUE));
        g_variant_builder_add (&props, "{sv}", "visible",
                               g_variant_new_boolean (TRUE));
    }
    else
    {
        g_variant_builder_add (&props, "{sv}", "children-display",
                               g_variant_new_string ("submenu"));
    }

    GVariantBuilder children;
    g_variant_builder_init (&children, G_VARIANT_TYPE ("av"));

    if (is_root)
    {
        g_variant_builder_add (&children, "v",
                               build_menu_item (MENU_ID_SHOW, "Show OTPClient", FALSE));
        g_variant_builder_add (&children, "v",
                               build_menu_item (MENU_ID_QUIT, "Quit", FALSE));
    }

    return g_variant_new ("(ia{sv}av)", id,
                           &props, &children);
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

    if (g_strcmp0 (method_name, "GetLayout") == 0)
    {
        GVariant *layout = build_menu_item (0, NULL, TRUE);
        g_dbus_method_invocation_return_value (invocation,
                                               g_variant_new ("(u@(ia{sv}av))", 1, layout));
    }
    else if (g_strcmp0 (method_name, "Event") == 0)
    {
        gint32 id;
        const gchar *event_id;
        /* "&s" borrows from `parameters`; plain "s" would hand back a dup that
         * nothing here frees. */
        g_variant_get (parameters, "(i&s@vu)", &id, &event_id, NULL, NULL);

        if (g_strcmp0 (event_id, "clicked") == 0)
        {
            if (id == MENU_ID_SHOW)
                show_window (td);
            else if (id == MENU_ID_QUIT)
                g_application_quit (G_APPLICATION (td->app));
        }

        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "AboutToShow") == 0)
    {
        g_dbus_method_invocation_return_value (invocation,
                                               g_variant_new ("(b)", FALSE));
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
    (void) error;
    (void) user_data;

    if (g_strcmp0 (property_name, "Version") == 0)
        return g_variant_new_uint32 (3);
    if (g_strcmp0 (property_name, "TextDirection") == 0)
        return g_variant_new_string ("ltr");
    if (g_strcmp0 (property_name, "Status") == 0)
        return g_variant_new_string ("normal");

    return NULL;
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

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
    (void) name;

    TrayData *td = user_data;
    /* Our own reference: the connection has to outlive a refused name request,
     * because that is exactly when the fallback below still needs it. */
    g_set_object (&td->connection, connection);

    GError *err = NULL;

    g_autoptr (GDBusNodeInfo) sni_info =
        g_dbus_node_info_new_for_xml (sni_introspection_xml, &err);
    if (err != NULL)
    {
        g_warning ("Failed to parse SNI introspection: %s", err->message);
        g_clear_error (&err);
        return;
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
        return;
    }

    g_autoptr (GDBusNodeInfo) menu_info =
        g_dbus_node_info_new_for_xml (dbusmenu_introspection_xml, &err);
    if (err != NULL)
    {
        g_warning ("Failed to parse dbusmenu introspection: %s", err->message);
        g_clear_error (&err);
        return;
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
    }
}

/* Every async callback below re-reads the `tray_data` singleton instead of
 * trusting user_data: cleanup NULLs it, so this doubles as a liveness check on
 * the pointer the call was issued with. */
static void
on_item_registered (GObject      *source,
                    GAsyncResult *res,
                    gpointer      user_data)
{
    (void) user_data;

    GError *err = NULL;
    g_autoptr (GVariant) reply =
        g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &err);

    /* A disable or an unpublish while the call was in flight clears
     * `publishing`, which makes this reply stale: acting on it would resurrect
     * an item we already tore down, hold included. */
    if (tray_data == NULL || !tray_data->publishing)
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
}

/* Ask the watcher to adopt the item, and listen to the answer: whether an icon
 * actually exists decides whether closing the window is allowed to hide it.
 * `service` is the bus name the item can be reached at, either our well-known
 * name or, where the bus refused to hand that out, the unique one. */
static void
tray_register_with_watcher (GDBusConnection *connection,
                            const gchar     *service)
{
    g_dbus_connection_call (connection,
                            WATCHER_BUS_NAME,
                            WATCHER_OBJECT_PATH,
                            "org.kde.StatusNotifierWatcher",
                            "RegisterStatusNotifierItem",
                            g_variant_new ("(s)", service),
                            NULL,
                            G_DBUS_CALL_FLAGS_NONE,
                            -1, NULL, on_item_registered, NULL);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
    (void) user_data;

    /* A NameAcquired that arrives after the fallback already registered us
     * would put a second item in the tray. */
    if (tray_data == NULL || tray_data->used_unique_name)
        return;

    tray_register_with_watcher (connection, name);
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

    /* GLib runs on_bus_acquired before this, so the item is already exported on
     * a live connection and only the name is missing. Sandboxes are the usual
     * reason: xdg-dbus-proxy answers RequestName with ServiceUnknown unless the
     * Flatpak manifest grants --own-name, and every sandboxed app is pid 2, so
     * the pid-derived name collides between apps anyway. The watcher does not
     * need a well-known name, so register the unique one instead, which is what
     * Qt's tray does (QDBusMenuConnection passes baseService()). */
    if (tray_data->publishing && !tray_data->published &&
        !tray_data->used_unique_name && tray_data->connection != NULL)
    {
        const gchar *unique = g_dbus_connection_get_unique_name (tray_data->connection);
        if (unique != NULL)
        {
            tray_data->used_unique_name = TRUE;
            g_info ("Could not own %s, registering the tray item as %s instead",
                    tray_data->bus_name, unique);
            tray_register_with_watcher (tray_data->connection, unique);
            return;
        }
    }

    g_info ("Lost bus name for StatusNotifierItem");
    tray_unpublish (tray_data);
}

static void
tray_publish (TrayData *td)
{
    if (td->publishing || td->published)
        return;
    if (!td->desired || td->host != TRAY_HOST_AVAILABLE)
        return;

    td->publishing = TRUE;
    /* DO_NOT_QUEUE: a pid-derived name is not worth waiting in line for, and
     * queueing is what would deliver a late NameAcquired on top of a fallback
     * registration. Two sandboxed apps both at pid 2 now each get an icon. */
    td->bus_name_id =
        g_bus_own_name (G_BUS_TYPE_SESSION,
                        td->bus_name,
                        G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
                        on_bus_acquired,
                        on_name_acquired,
                        on_name_lost,
                        td,
                        NULL);
}

static void
tray_unpublish (TrayData *td)
{
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
        g_clear_object (&td->connection);
    }

    if (td->bus_name_id != 0)
    {
        g_bus_unown_name (td->bus_name_id);
        td->bus_name_id = 0;
    }

    td->publishing = FALSE;
    td->published = FALSE;
    td->used_unique_name = FALSE;
    tray_sync_hold (td);

    /* The panel can go away (extension toggled off, shell restarted) while the
     * window is tucked into the tray. Bring it back rather than stranding it. */
    if (td->window_hidden)
        show_window (td);
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

    GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));
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
     * hold on to a stale Active entry. */
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
}

gboolean
otpclient_tray_is_available (void)
{
    return tray_data != NULL && tray_data->host != TRAY_HOST_UNAVAILABLE;
}

void
otpclient_tray_cleanup (OTPClientApplication *app)
{
    if (tray_data == NULL)
        return;

    if (tray_data->close_handler_id != 0)
    {
        GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));
        if (window != NULL)
            g_signal_handler_disconnect (window, tray_data->close_handler_id);
    }

    if (tray_data->host_signal_id != 0 && tray_data->watch_connection != NULL)
        g_dbus_connection_signal_unsubscribe (tray_data->watch_connection,
                                              tray_data->host_signal_id);
    g_clear_object (&tray_data->watch_connection);

    if (tray_data->watcher_watch_id != 0)
        g_bus_unwatch_name (tray_data->watcher_watch_id);

    /* Clears the hold too, so the teardown doesn't leave the app held. */
    tray_data->window_hidden = FALSE;
    tray_unpublish (tray_data);

    g_free (tray_data->bus_name);
    g_free (tray_data);
    tray_data = NULL;
}

#endif
