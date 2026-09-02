#ifdef ENABLE_MINIMIZE_TO_TRAY

#include <unistd.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include "tray.h"
#include "otpclient-application.h"

#define SNI_OBJECT_PATH    "/StatusNotifierItem"
#define DBUSMENU_OBJECT_PATH "/StatusNotifierMenu"

#define WATCHER_BUS_NAME     "org.kde.StatusNotifierWatcher"
#define WATCHER_OBJECT_PATH  "/StatusNotifierWatcher"

#define MENU_ID_ROOT  0
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
    "    <method name='GetGroupProperties'>"
    "      <arg type='ai' name='ids' direction='in'/>"
    "      <arg type='as' name='propertyNames' direction='in'/>"
    "      <arg type='a(ia{sv})' name='properties' direction='out'/>"
    "    </method>"
    "    <method name='GetProperty'>"
    "      <arg type='i' name='id' direction='in'/>"
    "      <arg type='s' name='name' direction='in'/>"
    "      <arg type='v' name='value' direction='out'/>"
    "    </method>"
    "    <method name='EventGroup'>"
    "      <arg type='a(isvu)' name='events' direction='in'/>"
    "      <arg type='ai' name='idErrors' direction='out'/>"
    "    </method>"
    "    <method name='AboutToShowGroup'>"
    "      <arg type='ai' name='ids' direction='in'/>"
    "      <arg type='ai' name='updatesNeeded' direction='out'/>"
    "      <arg type='ai' name='idErrors' direction='out'/>"
    "    </method>"
    "    <signal name='LayoutUpdated'>"
    "      <arg type='u' name='revision'/>"
    "      <arg type='i' name='parent'/>"
    "    </signal>"
    "    <signal name='ItemsPropertiesUpdated'>"
    "      <arg type='a(ia{sv})' name='updatedProps'/>"
    "      <arg type='a(ias)' name='removedProps'/>"
    "    </signal>"
    "    <signal name='ItemActivationRequested'>"
    "      <arg type='i' name='id'/>"
    "      <arg type='u' name='timestamp'/>"
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

/* NULL for anything that is not one of our leaves, which doubles as the
 * "does this id exist" test. Translated on each call: gettext owns the result
 * and it stays valid for the life of the process. */
static const gchar *
menu_label_for_id (gint32 id)
{
    switch (id)
    {
        case MENU_ID_SHOW: return _("Show OTPClient");
        case MENU_ID_QUIT: return _("Quit");
        default:           return NULL;
    }
}

static gboolean
menu_id_exists (gint32 id)
{
    return id == MENU_ID_ROOT || menu_label_for_id (id) != NULL;
}

/* The single source of truth for an item's properties, shared by GetLayout,
 * GetGroupProperties and GetProperty. It always emits the item's *full* set:
 * libdbusmenu's client replaces rather than merges what a properties reply
 * carries, so anything left out is actively removed from the item, and a
 * missing label falls back to the client's own "Label Empty" placeholder. */
static void
add_item_properties (GVariantBuilder *props,
                     gint32           id)
{
    if (id == MENU_ID_ROOT)
    {
        /* Only the root is a submenu. Claiming children-display on a leaf makes
         * libdbusmenu-gtk build an empty child menu for it and route clicks to
         * AboutToShow instead of activating it, i.e. a dead entry. */
        g_variant_builder_add (props, "{sv}", "children-display",
                               g_variant_new_string ("submenu"));
        return;
    }

    g_variant_builder_add (props, "{sv}", "type",
                           g_variant_new_string ("standard"));
    g_variant_builder_add (props, "{sv}", "label",
                           g_variant_new_string (menu_label_for_id (id)));
    g_variant_builder_add (props, "{sv}", "enabled",
                           g_variant_new_boolean (TRUE));
    g_variant_builder_add (props, "{sv}", "visible",
                           g_variant_new_boolean (TRUE));
}

static GVariant *
build_menu_item (gint32 id)
{
    GVariantBuilder props;
    g_variant_builder_init (&props, G_VARIANT_TYPE ("a{sv}"));
    add_item_properties (&props, id);

    GVariantBuilder children;
    g_variant_builder_init (&children, G_VARIANT_TYPE ("av"));

    if (id == MENU_ID_ROOT)
    {
        g_variant_builder_add (&children, "v", build_menu_item (MENU_ID_SHOW));
        g_variant_builder_add (&children, "v", build_menu_item (MENU_ID_QUIT));
    }

    return g_variant_new ("(ia{sv}av)", id, &props, &children);
}

static void
append_item_properties (GVariantBuilder *out,
                        gint32           id)
{
    if (!menu_id_exists (id))
        return;

    GVariantBuilder props;
    g_variant_builder_init (&props, G_VARIANT_TYPE ("a{sv}"));
    add_item_properties (&props, id);

    g_variant_builder_add (out, "(ia{sv})", id, &props);
}

/* propertyNames is deliberately ignored, here and in GetLayout: sending a
 * superset is always allowed, every studied client copes, and KDE depends on
 * the inline properties GetLayout returns. Honouring the filter is what would
 * break them. */
static GVariant *
build_group_properties (GVariant *ids)
{
    GVariantBuilder out;
    g_variant_builder_init (&out, G_VARIANT_TYPE ("a(ia{sv})"));

    if (g_variant_n_children (ids) == 0)
    {
        /* Per the spec, an empty id list means every item. */
        append_item_properties (&out, MENU_ID_ROOT);
        append_item_properties (&out, MENU_ID_SHOW);
        append_item_properties (&out, MENU_ID_QUIT);
    }
    else
    {
        GVariantIter iter;
        gint32 id;

        g_variant_iter_init (&iter, ids);
        while (g_variant_iter_next (&iter, "i", &id))
            append_item_properties (&out, id);
    }

    return g_variant_new ("(a(ia{sv}))", &out);
}

/* Returns a new reference, or NULL when the item or the property is unknown. */
static GVariant *
lookup_item_property (gint32       id,
                      const gchar *name)
{
    if (!menu_id_exists (id))
        return NULL;

    GVariantBuilder props;
    g_variant_builder_init (&props, G_VARIANT_TYPE ("a{sv}"));
    add_item_properties (&props, id);

    g_autoptr (GVariant) dict = g_variant_ref_sink (g_variant_builder_end (&props));

    return g_variant_lookup_value (dict, name, NULL);
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
dbusmenu_dispatch_event (TrayData    *td,
                         gint32       id,
                         const gchar *event_id)
{
    /* KDE also sends "opened" and "closed" around the popup; those are not
     * activations and must not trip the actions. */
    if (g_strcmp0 (event_id, "clicked") != 0)
        return;

    if (id == MENU_ID_SHOW)
        show_window (td);
    else if (id == MENU_ID_QUIT)
        g_idle_add (quit_in_idle, td->app);
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
        GVariant *layout = build_menu_item (MENU_ID_ROOT);
        g_dbus_method_invocation_return_value (invocation,
                                               g_variant_new ("(u@(ia{sv}av))", 1, layout));
    }
    else if (g_strcmp0 (method_name, "GetGroupProperties") == 0)
    {
        g_autoptr (GVariant) ids = g_variant_get_child_value (parameters, 0);
        g_dbus_method_invocation_return_value (invocation, build_group_properties (ids));
    }
    else if (g_strcmp0 (method_name, "GetProperty") == 0)
    {
        gint32 id;
        const gchar *name;
        g_variant_get (parameters, "(i&s)", &id, &name);

        g_autoptr (GVariant) value = lookup_item_property (id, name);
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

        dbusmenu_dispatch_event (td, id, event_id);

        g_dbus_method_invocation_return_value (invocation, NULL);
    }
    else if (g_strcmp0 (method_name, "EventGroup") == 0)
    {
        g_autoptr (GVariant) events = g_variant_get_child_value (parameters, 0);

        GVariantBuilder errors;
        g_variant_builder_init (&errors, G_VARIANT_TYPE ("ai"));

        GVariantIter iter;
        GVariant *child = NULL;

        g_variant_iter_init (&iter, events);
        /* Iterating by value keeps each event tuple alive while event_id
         * borrows from it. */
        while ((child = g_variant_iter_next_value (&iter)) != NULL)
        {
            gint32 id;
            const gchar *event_id;
            g_variant_get (child, "(i&s@vu)", &id, &event_id, NULL, NULL);

            if (menu_id_exists (id))
                dbusmenu_dispatch_event (td, id, event_id);
            else
                g_variant_builder_add (&errors, "i", id);

            g_variant_unref (child);
        }

        g_dbus_method_invocation_return_value (invocation,
                                               g_variant_new ("(ai)", &errors));
    }
    else if (g_strcmp0 (method_name, "AboutToShow") == 0)
    {
        g_dbus_method_invocation_return_value (invocation,
                                               g_variant_new ("(b)", FALSE));
    }
    else if (g_strcmp0 (method_name, "AboutToShowGroup") == 0)
    {
        g_autoptr (GVariant) ids = g_variant_get_child_value (parameters, 0);

        GVariantBuilder updates, errors;
        g_variant_builder_init (&updates, G_VARIANT_TYPE ("ai"));
        g_variant_builder_init (&errors, G_VARIANT_TYPE ("ai"));

        GVariantIter iter;
        gint32 id;

        g_variant_iter_init (&iter, ids);
        while (g_variant_iter_next (&iter, "i", &id))
        {
            if (!menu_id_exists (id))
                g_variant_builder_add (&errors, "i", id);
        }

        /* The menu is static, so no item ever needs a layout refresh before it
         * is shown and updatesNeeded stays empty. Note the reply is (aiai): a
         * mismatched type here would make GDBus log and send no reply at all,
         * hanging the panel for the full D-Bus timeout. */
        g_dbus_method_invocation_return_value (invocation,
                                               g_variant_new ("(aiai)", &updates, &errors));
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
