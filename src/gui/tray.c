#ifdef ENABLE_MINIMIZE_TO_TRAY

#include <gio/gio.h>
#include <gtk/gtk.h>
#include "tray.h"
#include "otpclient-application.h"

#define SNI_OBJECT_PATH    "/StatusNotifierItem"
#define DBUSMENU_OBJECT_PATH "/StatusNotifierMenu"

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

typedef struct
{
    OTPClientApplication *app;
    GDBusConnection *connection;
    guint sni_registration_id;
    guint menu_registration_id;
    guint bus_name_id;
    gulong close_handler_id;
    gchar *bus_name;
    gboolean active;
} TrayData;

static TrayData *tray_data = NULL;

static void
show_window (OTPClientApplication *app)
{
    GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));
    if (window != NULL)
    {
        gtk_widget_set_visible (GTK_WIDGET (window), TRUE);
        gtk_window_present (window);
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
        show_window (td->app);
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
        return g_variant_new_string (td->active ? "Active" : "Passive");
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
        g_variant_get (parameters, "(is@vu)", &id, &event_id, NULL, NULL);

        if (g_strcmp0 (event_id, "clicked") == 0)
        {
            if (id == MENU_ID_SHOW)
                show_window (td->app);
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

    if (td->active && otpclient_application_get_minimize_to_tray (td->app))
    {
        gtk_widget_set_visible (GTK_WIDGET (window), FALSE);
        return TRUE;
    }

    return FALSE;
}

/* --- Bus name acquired / registration --- */

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
    (void) name;

    TrayData *td = user_data;
    td->connection = connection;

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

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
    (void) user_data;

    /* Register with the StatusNotifierWatcher */
    g_dbus_connection_call (connection,
                            "org.kde.StatusNotifierWatcher",
                            "/StatusNotifierWatcher",
                            "org.kde.StatusNotifierWatcher",
                            "RegisterStatusNotifierItem",
                            g_variant_new ("(s)", name),
                            NULL,
                            G_DBUS_CALL_FLAGS_NONE,
                            -1, NULL, NULL, NULL);
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
    (void) connection;
    (void) name;
    (void) user_data;

    g_info ("Lost bus name for StatusNotifierItem");
}

/* --- Public API --- */

void
otpclient_tray_init (OTPClientApplication *app)
{
    if (tray_data != NULL)
        return;

    tray_data = g_new0 (TrayData, 1);
    tray_data->app = app;
    tray_data->active = otpclient_application_get_minimize_to_tray (app);

    tray_data->bus_name = g_strdup_printf ("org.kde.StatusNotifierItem-%d-1",
                                            getpid ());

    tray_data->bus_name_id =
        g_bus_own_name (G_BUS_TYPE_SESSION,
                        tray_data->bus_name,
                        G_BUS_NAME_OWNER_FLAGS_NONE,
                        on_bus_acquired,
                        on_name_acquired,
                        on_name_lost,
                        tray_data,
                        NULL);

    GtkWindow *window = gtk_application_get_active_window (GTK_APPLICATION (app));
    if (window != NULL)
    {
        tray_data->close_handler_id =
            g_signal_connect (window, "close-request",
                              G_CALLBACK (on_close_request), tray_data);
    }

    if (tray_data->active)
        g_application_hold (G_APPLICATION (app));
}

void
otpclient_tray_enable (OTPClientApplication *app)
{
    if (tray_data == NULL)
    {
        otpclient_tray_init (app);
        return;
    }

    if (!tray_data->active)
    {
        tray_data->active = TRUE;
        g_application_hold (G_APPLICATION (app));

        if (tray_data->connection != NULL)
        {
            g_dbus_connection_emit_signal (tray_data->connection,
                                           NULL,
                                           SNI_OBJECT_PATH,
                                           "org.kde.StatusNotifierItem",
                                           "NewStatus",
                                           g_variant_new ("(s)", "Active"),
                                           NULL);
        }
    }
}

void
otpclient_tray_disable (OTPClientApplication *app)
{
    if (tray_data == NULL || !tray_data->active)
        return;

    tray_data->active = FALSE;
    g_application_release (G_APPLICATION (app));

    if (tray_data->connection != NULL)
    {
        g_dbus_connection_emit_signal (tray_data->connection,
                                       NULL,
                                       SNI_OBJECT_PATH,
                                       "org.kde.StatusNotifierItem",
                                       "NewStatus",
                                       g_variant_new ("(s)", "Passive"),
                                       NULL);
    }
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

    if (tray_data->connection != NULL)
    {
        if (tray_data->sni_registration_id != 0)
            g_dbus_connection_unregister_object (tray_data->connection,
                                                  tray_data->sni_registration_id);
        if (tray_data->menu_registration_id != 0)
            g_dbus_connection_unregister_object (tray_data->connection,
                                                  tray_data->menu_registration_id);
    }

    if (tray_data->bus_name_id != 0)
        g_bus_unown_name (tray_data->bus_name_id);

    if (tray_data->active)
        g_application_release (G_APPLICATION (app));

    g_free (tray_data->bus_name);
    g_free (tray_data);
    tray_data = NULL;
}

#endif
