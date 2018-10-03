#pragma once

#include <gtk/gtk.h>
#include "db-misc.h"

#define NOTIFICATION_ID "otp-copied"

G_BEGIN_DECLS

enum _search_column_id { LABEL, ISSUER } SearchColumnID;

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