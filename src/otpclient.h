#pragma once

#include "db-misc.h"
#include "treeview.h"

G_BEGIN_DECLS

#define APP_NAME                "OTPClient"
#define APP_VERSION             "1.1.1"

#define HOTP_RATE_LIMIT_IN_SEC  3

void activate               (GtkApplication *app,
                             gpointer user_data);

int add_data_dialog         (GtkWidget *main_window,
                             DatabaseData *db_data,
                             GtkListStore *list_store);

G_END_DECLS
