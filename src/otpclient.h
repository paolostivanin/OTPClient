#pragma once

#include "db-misc.h"
#include "treeview.h"

G_BEGIN_DECLS

#define APP_NAME    "OTPClient"
#define APP_VERSION "0.99.5 (1.0-alpha5)"

#define DB_FILE_NAME     "otpclient-db.enc"

void activate               (GtkApplication *app,
                             gpointer user_data);

void show_message_dialog    (GtkWidget *parent,
                             const gchar *message,
                             GtkMessageType message_type);

int add_data_dialog         (GtkWidget *main_window,
                             DatabaseData *db_data,
                             GtkListStore *list_store);

G_END_DECLS