/* CTest runs each scenario in its own process with a private bus and display. */
#include "review-fixture.h"
#include "otpclient-application.h"
#include "otpclient-window.h"
#include "lock-app.h"
#include "tray.h"
#include "version.h"

static OTPClientApplication *app;
static GtkWindow *window;
static GDBusConnection *bus;
static guint registrations;
static gchar *item_sender;
static gboolean hold_registration;
static GDBusMethodInvocation *pending_registration;
static guint shown_count;

static struct {
    const gchar *name;
    const gchar *path;
    gboolean active;
    gboolean hold_reply;
    GDBusMethodInvocation *pending;
    guint object_id;
} screens[] = {
    { .name = "org.gnome.ScreenSaver", .path = "/org/gnome/ScreenSaver" },
    { .name = "org.cinnamon.ScreenSaver", .path = "/org/cinnamon/ScreenSaver" },
    { .name = "org.freedesktop.ScreenSaver", .path = "/org/freedesktop/ScreenSaver" },
};

static const gchar watcher_xml[] =
    "<node><interface name='org.kde.StatusNotifierWatcher'>"
    "<method name='RegisterStatusNotifierItem'><arg type='s' direction='in'/></method>"
    "<property name='IsStatusNotifierHostRegistered' type='b' access='read'/>"
    "</interface></node>";
static const gchar screen_xml[] =
    "<node><interface name='%s'>"
    "<method name='GetActive'><arg type='b' direction='out'/></method>"
    "<signal name='ActiveChanged'><arg type='b'/></signal>"
    "</interface></node>";

static void
iterate_for (guint milliseconds)
{
    gint64 end = g_get_monotonic_time () + milliseconds * 1000;
    do {
        for (guint i = 0; i < 100 && g_main_context_iteration (NULL, FALSE); i++);
        g_usleep (1000);
    } while (g_get_monotonic_time () < end);
}

static void
set_name (const gchar *name, gboolean own)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GVariant) reply = g_dbus_connection_call_sync (
        bus, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
        own ? "RequestName" : "ReleaseName",
        own ? g_variant_new ("(su)", name, 0u) : g_variant_new ("(s)", name),
        G_VARIANT_TYPE ("(u)"), G_DBUS_CALL_FLAGS_NONE, 2000, NULL, &error);
    g_assert_no_error (error);
    guint result;
    g_variant_get (reply, "(u)", &result);
    g_assert_cmpuint (result, ==, 1);
}

static void
method_call (GDBusConnection *connection, const gchar *sender, const gchar *path,
             const gchar *interface, const gchar *method, GVariant *parameters,
             GDBusMethodInvocation *invocation, gpointer data)
{
    (void) connection; (void) path; (void) parameters; (void) data;
    if (g_str_equal (method, "GetActive")) {
        for (guint i = 0; i < G_N_ELEMENTS (screens); i++) {
            if (!g_str_equal (interface, screens[i].name))
                continue;
            if (screens[i].hold_reply) {
                g_assert_null (screens[i].pending);
                screens[i].pending = g_object_ref (invocation);
            } else {
                g_dbus_method_invocation_return_value (invocation, g_variant_new ("(b)", screens[i].active));
            }
            return;
        }
        g_assert_not_reached ();
    } else {
        g_assert_cmpstr (method, ==, "RegisterStatusNotifierItem");
        registrations++;
        g_free (item_sender);
        item_sender = g_strdup (sender);
        if (hold_registration) {
            g_assert_null (pending_registration);
            pending_registration = g_object_ref (invocation);
        } else {
            g_dbus_method_invocation_return_value (invocation, NULL);
        }
    }
}

static GVariant *
get_property (GDBusConnection *connection, const gchar *sender, const gchar *path,
              const gchar *interface, const gchar *property, GError **error, gpointer data)
{
    (void) connection; (void) sender; (void) path; (void) interface; (void) error; (void) data;
    g_assert_cmpstr (property, ==, "IsStatusNotifierHostRegistered");
    return g_variant_new_boolean (TRUE);
}

static const GDBusInterfaceVTable vtable = {
    .method_call = method_call, .get_property = get_property,
};

static guint
export_object (const gchar *path, const gchar *xml)
{
    g_autoptr (GError) error = NULL;
    g_autoptr (GDBusNodeInfo) node = g_dbus_node_info_new_for_xml (xml, &error);
    g_assert_no_error (error);
    guint id = g_dbus_connection_register_object (bus, path, node->interfaces[0],
                                                  &vtable, NULL, NULL, &error);
    g_assert_no_error (error);
    g_assert_cmpuint (id, >, 0);
    return id;
}

static void
emit_screen_signal (guint index, GVariant *parameters)
{
    g_autoptr (GError) error = NULL;
    g_assert_true (g_dbus_connection_emit_signal (bus, NULL, screens[index].path,
        screens[index].name, "ActiveChanged", parameters, &error));
    g_assert_no_error (error);
    iterate_for (100);
}

static void
set_screen_active (gboolean active)
{
    screens[0].active = active;
    emit_screen_signal (0, g_variant_new ("(b)", active));
}

static void
window_visibility_changed (GtkWidget *widget, GParamSpec *pspec, gpointer data)
{
    (void) pspec; (void) data;
    if (gtk_widget_get_visible (widget))
        shown_count++;
}

static void
wait_for_registration (guint count)
{
    gint64 end = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;
    while (registrations < count && g_get_monotonic_time () < end)
        iterate_for (10);
    g_assert_cmpuint (registrations, ==, count);
    iterate_for (100);
}

static void
hide_window (void)
{
    gboolean handled = FALSE;
    g_signal_emit_by_name (window, "close-request", &handled);
    g_assert_true (handled);
    g_assert_false (gtk_widget_get_visible (GTK_WIDGET (window)));
    shown_count = 0;
}

static void
test_locked (gconstpointer data)
{
    guint flags = GPOINTER_TO_UINT (data);
    gboolean auto_lock = (flags & 1) != 0;
    ReviewFixture fixture;
    review_fixture_init (&fixture);
    otpclient_application_set_db_data (app, database_data_ref (fixture.db));
    otpclient_application_set_app_locked (app, FALSE);
    otpclient_application_set_auto_lock (app, auto_lock);
    otpclient_application_set_minimize_to_tray (app, TRUE);
    wait_for_registration (1);
    gtk_window_present (window);
    hide_window ();
    if (flags & 2) {
        set_name ("org.kde.StatusNotifierWatcher", FALSE);
        iterate_for (300);
        set_screen_active (TRUE);
    } else {
        set_screen_active (TRUE);
        set_name ("org.kde.StatusNotifierWatcher", FALSE);
    }
    iterate_for (4300);
    g_assert_false (gtk_widget_get_visible (GTK_WIDGET (window)));
    g_assert_cmpint (otpclient_application_get_app_locked (app), ==, auto_lock);
    if (auto_lock)
        g_assert_null (fixture.db->in_memory_json_data);
    else
        g_assert_nonnull (fixture.db->in_memory_json_data);
    if (!(flags & 4))
        set_screen_active (FALSE);
    set_name ("org.kde.StatusNotifierWatcher", TRUE);
    wait_for_registration (2);
    if (flags & 4)
        set_screen_active (FALSE);
    iterate_for (4300);
    g_assert_false (gtk_widget_get_visible (GTK_WIDGET (window)));
    g_assert_cmpuint (shown_count, ==, 0);
    g_assert_cmpint (otpclient_application_get_app_locked (app), ==, auto_lock);
    review_fixture_clear (&fixture);
}

static void
enable_and_hide (void)
{
    otpclient_application_set_auto_lock (app, FALSE);
    otpclient_application_set_minimize_to_tray (app, TRUE);
    wait_for_registration (1);
    gtk_window_present (window);
    hide_window ();
}

static void
test_missing_tray (void)
{
    enable_and_hide ();
    set_screen_active (TRUE);
    set_name ("org.kde.StatusNotifierWatcher", FALSE);
    iterate_for (4300);
    g_assert_cmpuint (shown_count, ==, 0);
    set_screen_active (FALSE);
    /* Repeated unlocks must not keep extending the deadline. */
    for (guint i = 0; i < 5; i++) {
        iterate_for (800);
        set_screen_active (FALSE);
    }
    g_assert_true (gtk_widget_get_visible (GTK_WIDGET (window)));
    g_assert_cmpuint (shown_count, ==, 1);
}

static void
test_unlocked_tray_loss (void)
{
    enable_and_hide ();
    set_name ("org.kde.StatusNotifierWatcher", FALSE);
    iterate_for (4300);
    g_assert_true (gtk_widget_get_visible (GTK_WIDGET (window)));
    g_assert_cmpuint (shown_count, ==, 1);
}

static void
test_startup_deadline (void)
{
    otpclient_application_set_auto_lock (app, FALSE);
    hold_registration = TRUE;
    otpclient_application_set_minimize_to_tray (app, TRUE);
    wait_for_registration (1);
    gtk_widget_set_visible (GTK_WIDGET (window), FALSE);
    shown_count = 0;
    otpclient_tray_begin_hidden (app);
    iterate_for (100);
    set_screen_active (TRUE);
    iterate_for (11300);
    g_assert_cmpuint (shown_count, ==, 0);
    set_screen_active (FALSE);
    iterate_for (4300);
    g_assert_cmpuint (shown_count, ==, 0);
    iterate_for (7000);
    g_assert_cmpuint (shown_count, ==, 1);
}

static void
wait_for_query (guint index)
{
    gint64 end = g_get_monotonic_time () + 2 * G_USEC_PER_SEC;
    while (screens[index].pending == NULL && g_get_monotonic_time () < end)
        iterate_for (10);
    g_assert_nonnull (screens[index].pending);
}

static void
finish_query (guint index, gboolean active)
{
    g_dbus_method_invocation_return_value (screens[index].pending, g_variant_new ("(b)", active));
    g_clear_object (&screens[index].pending);
    iterate_for (100);
}

static void
test_initial_and_stale_state (void)
{
    enable_and_hide ();
    /* Simulate starting the monitor with the screen already locked. */
    lock_app_cleanup (app);
    screens[0].active = TRUE;
    lock_app_init_dbus_watchers (app);
    iterate_for (200);
    g_assert_true (lock_app_get_session_locked (app));
    set_name ("org.kde.StatusNotifierWatcher", FALSE);
    iterate_for (4300);
    g_assert_cmpuint (shown_count, ==, 0);

    /* A stale initial FALSE must not override a newer lock signal. */
    lock_app_cleanup (app);
    screens[0].hold_reply = TRUE;
    lock_app_init_dbus_watchers (app);
    wait_for_query (0);
    set_screen_active (TRUE);
    finish_query (0, FALSE);
    g_assert_true (lock_app_get_session_locked (app));

    /* Nor may an initial TRUE undo a later unlock. */
    lock_app_cleanup (app);
    lock_app_init_dbus_watchers (app);
    wait_for_query (0);
    set_screen_active (FALSE);
    finish_query (0, TRUE);
    g_assert_false (lock_app_get_session_locked (app));
}

static void
test_multiple_services (void)
{
    enable_and_hide ();
    set_name (screens[1].name, TRUE);
    set_name (screens[2].name, TRUE);
    iterate_for (200);
    set_screen_active (TRUE);
    emit_screen_signal (1, g_variant_new ("(b)", TRUE));
    set_screen_active (FALSE);
    g_assert_true (lock_app_get_session_locked (app));
    emit_screen_signal (2, g_variant_new ("(b)", FALSE));
    g_assert_true (lock_app_get_session_locked (app));
    emit_screen_signal (1, g_variant_new ("(s)", "malformed"));
    g_assert_true (lock_app_get_session_locked (app));
    set_name ("org.kde.StatusNotifierWatcher", FALSE);
    iterate_for (4300);
    g_assert_cmpuint (shown_count, ==, 0);
    set_name (screens[1].name, FALSE);
    iterate_for (4300);
    g_assert_false (lock_app_get_session_locked (app));
    g_assert_cmpuint (shown_count, ==, 1);

    /* A service reappearing reports its current state without a new signal. */
    screens[1].active = TRUE;
    set_name (screens[1].name, TRUE);
    iterate_for (200);
    g_assert_true (lock_app_get_session_locked (app));
}

static void
test_query_owner_change (void)
{
    otpclient_application_set_auto_lock (app, FALSE);
    screens[1].hold_reply = TRUE;
    set_name (screens[1].name, TRUE);
    wait_for_query (1);
    set_name (screens[1].name, FALSE);
    iterate_for (100);
    finish_query (1, TRUE);
    g_assert_false (lock_app_get_session_locked (app));
    /* Missing/unsupported GetActive must not disable signal monitoring. */
    set_name (screens[1].name, TRUE);
    wait_for_query (1);
    g_dbus_method_invocation_return_dbus_error (screens[1].pending,
        "org.freedesktop.DBus.Error.UnknownMethod", "GetActive unavailable");
    g_clear_object (&screens[1].pending);
    iterate_for (100);
    g_assert_false (lock_app_get_session_locked (app));
    emit_screen_signal (1, g_variant_new ("(b)", TRUE));
    g_assert_true (lock_app_get_session_locked (app));
}

static void
activation_done (GObject *source, GAsyncResult *result, gpointer data)
{
    gboolean *done = data;
    g_autoptr (GError) error = NULL;
    g_autoptr (GVariant) reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &error);
    g_assert_no_error (error);
    g_assert_nonnull (reply);
    *done = TRUE;
}

static void
test_explicit_activation (void)
{
    enable_and_hide ();
    set_screen_active (TRUE);
    gboolean done = FALSE;
    g_dbus_connection_call (bus, item_sender, "/StatusNotifierItem", "org.kde.StatusNotifierItem",
        "Activate", g_variant_new ("(ii)", 0, 0), G_VARIANT_TYPE_UNIT,
        G_DBUS_CALL_FLAGS_NONE, 2000, NULL, activation_done, &done);
    gint64 end = g_get_monotonic_time () + 3 * G_USEC_PER_SEC;
    while (!done && g_get_monotonic_time () < end)
        iterate_for (10);
    g_assert_true (done);
    g_assert_cmpuint (shown_count, ==, 1);
    hide_window ();
    set_name ("org.kde.StatusNotifierWatcher", FALSE);
    iterate_for (100);
    otpclient_application_set_minimize_to_tray (app, FALSE);
    g_assert_cmpuint (shown_count, ==, 1);
    set_screen_active (FALSE);
    iterate_for (4300);
    g_assert_cmpuint (shown_count, ==, 1);
}

static void
test_cleanup (void)
{
    enable_and_hide ();
    set_name ("org.kde.StatusNotifierWatcher", FALSE);
    iterate_for (100);
    screens[1].hold_reply = TRUE;
    set_name (screens[1].name, TRUE);
    wait_for_query (1);
    otpclient_tray_cleanup (app);
    lock_app_cleanup (app);
    /* Complete an old query after a fresh monitor has already been created. */
    set_name (screens[1].name, FALSE);
    lock_app_init_dbus_watchers (app);
    finish_query (1, TRUE);
    g_assert_false (lock_app_get_session_locked (app));
    iterate_for (4300);
    g_assert_cmpuint (shown_count, ==, 0);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    gint32 memory = 0;
    set_memlock_value (&memory);
    g_autofree gchar *init_error = init_libs (memory);
    g_assert_null (init_error);
    g_log_set_always_fatal (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
    g_autoptr (GError) error = NULL;
    bus = g_dbus_connection_new_for_address_sync (g_getenv ("DBUS_SESSION_BUS_ADDRESS"),
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &error);
    g_assert_no_error (error);
    guint watcher_id = export_object ("/StatusNotifierWatcher", watcher_xml);
    for (guint i = 0; i < G_N_ELEMENTS (screens); i++) {
        g_autofree gchar *xml = g_strdup_printf (screen_xml, screens[i].name);
        screens[i].object_id = export_object (screens[i].path, xml);
    }
    set_name ("org.kde.StatusNotifierWatcher", TRUE);
    set_name ("org.gnome.ScreenSaver", TRUE);
    g_autoptr (GSettings) settings = g_settings_new ("com.github.paolostivanin.OTPClient");
    g_settings_set_string (settings, "last-seen-version", PROJECT_VER);
    app = otpclient_application_new ();
    g_application_set_flags (G_APPLICATION (app), G_APPLICATION_NON_UNIQUE);
    g_assert_true (g_application_register (G_APPLICATION (app), NULL, &error));
    g_assert_no_error (error);
    window = otpclient_application_get_window (app);
    g_signal_connect (window, "notify::visible", G_CALLBACK (window_visibility_changed), NULL);
    g_object_set (gtk_settings_get_default (), "gtk-enable-animations", FALSE, NULL);
    iterate_for (100);
    g_test_add_data_func ("/tray-session/locked-auto-lock-off", GINT_TO_POINTER (FALSE), test_locked);
    g_test_add_data_func ("/tray-session/locked-auto-lock-on", GINT_TO_POINTER (TRUE), test_locked);
    g_test_add_data_func ("/tray-session/tray-loss-before-lock", GUINT_TO_POINTER (2), test_locked);
    g_test_add_data_func ("/tray-session/tray-return-before-unlock", GUINT_TO_POINTER (4), test_locked);
    g_test_add_func ("/tray-session/missing-tray", test_missing_tray);
    g_test_add_func ("/tray-session/unlocked-tray-loss", test_unlocked_tray_loss);
    g_test_add_func ("/tray-session/startup-deadline", test_startup_deadline);
    g_test_add_func ("/tray-session/initial-and-stale-state", test_initial_and_stale_state);
    g_test_add_func ("/tray-session/multiple-services", test_multiple_services);
    g_test_add_func ("/tray-session/query-owner-change", test_query_owner_change);
    g_test_add_func ("/tray-session/explicit-activation", test_explicit_activation);
    g_test_add_func ("/tray-session/cleanup", test_cleanup);
    int result = g_test_run ();
    otpclient_tray_cleanup (app);
    lock_app_cleanup (app);
    gtk_window_destroy (window);
    g_object_unref (app);
    g_dbus_connection_unregister_object (bus, watcher_id);
    for (guint i = 0; i < G_N_ELEMENTS (screens); i++) {
        if (screens[i].pending != NULL) {
            g_dbus_method_invocation_return_value (screens[i].pending, g_variant_new ("(b)", FALSE));
            g_clear_object (&screens[i].pending);
        }
        g_dbus_connection_unregister_object (bus, screens[i].object_id);
    }
    if (pending_registration != NULL) {
        g_dbus_method_invocation_return_value (pending_registration, NULL);
        g_clear_object (&pending_registration);
    }
    iterate_for (100);
    g_dbus_connection_close_sync (bus, NULL, NULL);
    g_object_unref (bus);
    g_free (item_sender);
    return result;
}
