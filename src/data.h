#pragma once

#include <gtk/gtk.h>
#include <jansson.h>

#define DBUS_SERVICES 4

G_BEGIN_DECLS

typedef struct _db_data {
    gchar *db_path;

    gchar *key;

    json_t *json_data;

    GSList *objects_hash;

    GSList *data_to_add;

    gint32 max_file_size_from_memlock;

    gchar *last_hotp;
    GDateTime *last_hotp_update;
} DatabaseData;


typedef struct _app_data_t {
    GtkBuilder *builder;

    GtkWidget *main_window;
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

    GDateTime *last_user_activity;

    GtkWidget *diag_rcdb;
    GtkFileChooserAction open_db_file_action;
} AppData;

G_END_DECLS