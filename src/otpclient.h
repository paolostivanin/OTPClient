#pragma once

#include "db-misc.h"
#include "treeview.h"

G_BEGIN_DECLS

#define APP_NAME                "OTPClient"
#define APP_VERSION             "1.2.0"

#define HOTP_RATE_LIMIT_IN_SEC  3

void activate               (GtkApplication *app,
                             gpointer        user_data);

void add_data_dialog        (GSimpleAction  *simple,
                             GVariant       *parameter,
                             gpointer        user_data);

void webcam_cb              (GSimpleAction  *simple,
                             GVariant       *parameter,
                             gpointer        user_data);

void screenshot_cb          (GSimpleAction  *simple,
                             GVariant       *parameter,
                             gpointer        user_data);

void select_photo_cb        (GSimpleAction  *simple,
                             GVariant       *parameter,
                             gpointer        user_data);

void edit_selected_rows     (GSimpleAction  *simple,
                             GVariant       *parameter,
                             gpointer        user_data);

G_END_DECLS
