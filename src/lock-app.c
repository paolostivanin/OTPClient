#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include "data.h"
#include "get-builder.h"
#include "gui-common.h"
#include "message-dialogs.h"
#include "otpclient.h"
#include "lock-app.h"


static void
lock_app (GtkWidget *w __attribute__((unused)),
          gpointer user_data)
{
    AppData *app_data = (AppData *)user_data;

    app_data->app_locked = TRUE;

    g_signal_emit_by_name (app_data->tree_view, "hide-all-otps");

    GtkBuilder *builder = get_builder_from_partial_path (UI_PARTIAL_PATH);

    GtkWidget *dialog = GTK_WIDGET(gtk_builder_get_object (builder, "unlock_pwd_diag_id"));
    GtkWidget *pwd_entry = GTK_WIDGET(gtk_builder_get_object (builder, "unlock_entry_id"));

    g_signal_connect (pwd_entry, "icon-press", G_CALLBACK (icon_press_cb), NULL);
    g_signal_connect (pwd_entry, "activate", G_CALLBACK (send_ok_cb), NULL);

    gtk_window_set_transient_for (GTK_WINDOW(dialog), GTK_WINDOW(app_data->main_window));

    gtk_widget_show_all (dialog);

    gint ret;
    gboolean retry = FALSE;
    do {
        ret = gtk_dialog_run (GTK_DIALOG(dialog));
        if (ret == GTK_RESPONSE_OK) {
            if (g_strcmp0 (app_data->db_data->key, gtk_entry_get_text (GTK_ENTRY(pwd_entry))) != 0) {
                show_message_dialog (dialog, "The password is wrong, please try again.", GTK_MESSAGE_ERROR);
                gtk_entry_set_text (GTK_ENTRY(pwd_entry), "");
                retry = TRUE;
            } else {
                retry = FALSE;
                app_data->app_locked = FALSE;
                app_data->last_user_activity = g_date_time_new_now_local ();
                app_data->source_id_last_activity = g_timeout_add_seconds (1, check_inactivity, app_data);
                gtk_widget_destroy (dialog);
                g_object_unref (builder);
            }
        } else {
            gtk_widget_destroy (dialog);
            g_object_unref (builder);
            GtkApplication *app = gtk_window_get_application (GTK_WINDOW (app_data->main_window));
            destroy_cb (app_data->main_window, app_data);
            g_application_quit (G_APPLICATION(app));
        }
    } while (ret == GTK_RESPONSE_OK && retry == TRUE);
}


static void
signal_triggered_cb (GDBusConnection *connection __attribute__((unused)),
                     const gchar *sender_name    __attribute__((unused)),
                     const gchar *object_path    __attribute__((unused)),
                     const gchar *interface_name __attribute__((unused)),
                     const gchar *signal_name    __attribute__((unused)),
                     GVariant *parameters,
                     gpointer user_data)
{
    AppData *app_data = (AppData *)user_data;
    gboolean is_screen_locked;
    g_variant_get (parameters, "(b)", &is_screen_locked);
    if (is_screen_locked == TRUE && app_data->app_locked == FALSE && app_data->auto_lock == TRUE) {
        lock_app (NULL, app_data);
    }
}


gboolean
check_inactivity (gpointer user_data)
{
    AppData *app_data = (AppData *)user_data;
    if (app_data->inactivity_timeout > 0 && app_data->app_locked == FALSE) {
        GDateTime *now = g_date_time_new_now_local ();
        GTimeSpan diff = g_date_time_difference (now, app_data->last_user_activity);
        if (diff >= (G_USEC_PER_SEC * (GTimeSpan)app_data->inactivity_timeout)) {
            g_signal_emit_by_name (app_data->main_window, "lock-app");
            g_date_time_unref (now);
            return FALSE;
        }
        g_date_time_unref (now);
    }
    return TRUE;
}


void
setup_dbus_listener (AppData *app_data)
{
    g_signal_new ("lock-app", G_TYPE_OBJECT, G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    g_signal_connect (app_data->main_window, "lock-app", G_CALLBACK(lock_app), app_data);

    GtkBindingSet *binding_set = gtk_binding_set_by_class (GTK_WIDGET_GET_CLASS (app_data->main_window));
    gtk_binding_entry_add_signal (binding_set, GDK_KEY_l, GDK_CONTROL_MASK, "lock-app", 0);

    //const gchar *services[] = { "org.cinnamon.ScreenSaver", "org.freedesktop.ScreenSaver", "org.gnome.ScreenSaver", "com.canonical.Unity" };
    const gchar *paths[] = { "/org/cinnamon/ScreenSaver", "/org/freedesktop/ScreenSaver", "/org/gnome/ScreenSaver", "/com/canonical/Unity/Session" };
    const gchar *interfaces[] = { "org.cinnamon.ScreenSaver", "org.freedesktop.ScreenSaver", "org.gnome.ScreenSaver", "com.canonical.Unity.Session" };
    const gchar *signal_names[] = { "ActiveChanged", "ActiveChanged", "ActiveChanged", "Locked" };

    app_data->connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);

    for (guint i = 0; i < DBUS_SERVICES; i++) {
        app_data->subscription_ids[i] = g_dbus_connection_signal_subscribe (app_data->connection, interfaces[i], interfaces[i], signal_names[i], paths[i],
                                                                            NULL, G_DBUS_SIGNAL_FLAGS_NONE, signal_triggered_cb, app_data, NULL);
    }
}