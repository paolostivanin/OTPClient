#ifdef ENABLE_MINIMIZE_TO_TRAY
#include <gtk/gtk.h>
#include <glib.h>
#include <libayatana-appindicator/app-indicator.h>
#include "data.h"

gboolean hide_to_tray(GtkWidget *widget, GdkEvent *event __attribute__((unused)), gpointer user_data) {
    AppData *app_data = (AppData*) user_data;
    if (app_data->use_tray) {
        gtk_widget_hide(widget);
        return TRUE; // Prevent the app shutdown, so it just minimizes
    }
    return FALSE;
}

void quit_app(GtkWidget *widget __attribute__((unused)), gpointer user_data __attribute__((unused))) {
    GApplication *app = g_application_get_default();
    g_application_quit(G_APPLICATION(app));
}

void show_main_window(GtkMenuItem *item __attribute__((unused)), gpointer user_data) {
    AppData *app_data = (AppData*) user_data;
    gtk_widget_show_all(GTK_WIDGET(app_data->main_window));
    gtk_window_present(GTK_WINDOW(app_data->main_window));
}

void init_tray_icon(AppData *app_data) {
    gchar *icon_path = g_build_filename(DESTINATION, "share/icons/hicolor/scalable/apps/com.github.paolostivanin.OTPClient.svg", NULL);
    app_data->indicator = app_indicator_new("otpclient", icon_path, APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    g_free (icon_path);
    app_indicator_set_status(app_data->indicator, APP_INDICATOR_STATUS_ACTIVE);

    // Create a menu for the system tray icon
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *show_item = gtk_menu_item_new_with_label("Show OTPClient");
    g_signal_connect(show_item, "activate", G_CALLBACK(show_main_window), app_data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), show_item);

    GtkWidget *quit_item = gtk_menu_item_new_with_label("Quit");
    g_signal_connect(quit_item, "activate", G_CALLBACK(quit_app), NULL);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), quit_item);

    gtk_widget_show_all(menu);
    app_indicator_set_menu( app_data->indicator, GTK_MENU(menu));

    g_signal_connect(app_data->main_window, "delete-event", G_CALLBACK(hide_to_tray), app_data);
}

void switch_tray_use(AppData *app_data) {
    if (app_data->use_tray) {
        if (app_data->indicator == NULL) {
            init_tray_icon(app_data);    
        } else {
            app_indicator_set_status(app_data->indicator, APP_INDICATOR_STATUS_ACTIVE);
        }
    } else {
        app_indicator_set_status(app_data->indicator, APP_INDICATOR_STATUS_PASSIVE);
    }
}
#endif
