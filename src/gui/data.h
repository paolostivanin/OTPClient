#pragma once

#include <gtk/gtk.h>
#ifdef ENABLE_MINIMIZE_TO_TRAY
#include <libayatana-appindicator/app-indicator.h>
#endif
#include <jansson.h>
#include "../common/db-common.h"

#define DBUS_SERVICES 4

G_BEGIN_DECLS

typedef struct app_data_t {
    GtkBuilder *builder;
    GtkBuilder *add_popover_builder;
    GtkBuilder *settings_popover_builder;

    GtkWidget *main_window;
    GtkTreeView *tree_view;
    #ifdef ENABLE_MINIMIZE_TO_TRAY
    AppIndicator *indicator;
    #endif

    GtkClipboard *clipboard;

    gboolean show_next_otp;
    gboolean disable_notifications;
    gint search_column;
    gboolean auto_lock;
    gint inactivity_timeout;

    GtkCssProvider *delbtn_css_provider;
    GtkCssProvider *tv_css_provider;

    GNotification *notification;

    guint source_id;
    guint source_id_last_activity;

    DatabaseData *db_data;

    GDBusConnection *connection;
    guint subscription_ids[DBUS_SERVICES];

    gboolean app_locked;

    gboolean use_dark_theme;

    gboolean is_reorder_active;

    gboolean use_secret_service;

    gboolean use_tray;

    GDateTime *last_user_activity;

    GtkWidget *diag_rcdb;
    GtkFileChooserAction open_db_file_action;
} AppData;


typedef struct node_info_t {
    guint hash;
    guint newpos;
} NodeInfo;

G_END_DECLS
