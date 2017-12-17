#pragma once

#include "db-misc.h"
#include "treeview.h"

G_BEGIN_DECLS

#define APP_NAME                "OTPClient"
#define APP_VERSION             "1.0.0"

#define HOTP_RATE_LIMIT_IN_SEC  3
#define MAX_FILE_SIZE           1048576  // 1 MiB should be more than enough for such content.

void activate               (GtkApplication *app,
                             gpointer user_data);

int add_data_dialog         (GtkWidget *main_window,
                             DatabaseData *db_data,
                             GtkListStore *list_store);

G_END_DECLS