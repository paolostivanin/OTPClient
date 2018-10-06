#pragma once

#include <gtk/gtk.h>
#include <jansson.h>

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
    GtkWidget *main_window;
    GtkTreeView *tree_view;

    GtkClipboard *clipboard;
    
    gboolean show_next_otp;
    gboolean disable_notifications;
    gint search_column;

    GtkCssProvider *css_provider;

    GNotification *notification;

    guint source_id;

    DatabaseData *db_data;
} AppData;

G_END_DECLS