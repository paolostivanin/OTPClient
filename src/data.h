#pragma once

#include <gtk/gtk.h>
#include <jansson.h>

#define DBUS_SERVICES 4

G_BEGIN_DECLS

typedef struct db_data_t {
    gchar *db_path;

    gchar *key;

    json_t *json_data;

    GSList *objects_hash;

    GSList *data_to_add;

    gint32 max_file_size_from_memlock;

    gchar *last_hotp;
    GDateTime *last_hotp_update;

    gboolean key_stored;
} DatabaseData;


typedef struct app_data_t {
    GtkBuilder *builder;

    GtkWidget *main_window;
    GtkWidget *info_bar;
    GtkTreeView *tree_view;

    GtkClipboard *clipboard;

    gboolean show_next_otp;
    gboolean disable_notifications;
    gint search_column;
    gboolean auto_lock;
    gint inactivity_timeout;

    GtkCssProvider *css_provider;

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

    GDateTime *last_user_activity;

    GtkWidget *diag_rcdb;
    GtkFileChooserAction open_db_file_action;
} AppData;


typedef struct node_info_t {
    guint hash;
    guint newpos;
} NodeInfo;

G_END_DECLS
