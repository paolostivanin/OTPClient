#pragma once

#include "db-misc.h"
#include "treeview.h"

G_BEGIN_DECLS

#define APP_NAME                "OTPClient"
#define APP_VERSION             "1.2.0-dev"

#define HOTP_RATE_LIMIT_IN_SEC  3

void activate               (GtkApplication *app,
                             gpointer        user_data);

void add_data_dialog        (GSimpleAction  *simple,
                             GVariant       *parameter,
                             gpointer        user_data);

void webcam_cb              (GSimpleAction  *simple,
                             GVariant       *parameter,
                             gpointer        user_data);

G_END_DECLS
