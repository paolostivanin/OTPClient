#pragma once

#include "treeview.h"

G_BEGIN_DECLS

#define APP_NAME                "OTPClient"
#define APP_VERSION             "1.4.1"

#define HOTP_RATE_LIMIT_IN_SEC  3

#define NOTIFICATION_ID "otp-copied"

void activate              (GtkApplication *app,
                            gpointer        user_data);

void add_data_dialog       (GSimpleAction  *simple,
                            GVariant       *parameter,
                            gpointer        user_data);

void webcam_cb             (GSimpleAction  *simple,
                            GVariant       *parameter,
                            gpointer        user_data);

void screenshot_cb         (GSimpleAction  *simple,
                            GVariant       *parameter,
                            gpointer        user_data);

void select_photo_cb       (GSimpleAction  *simple,
                            GVariant       *parameter,
                            gpointer        user_data);

void edit_selected_row_cb  (GSimpleAction  *simple,
                            GVariant       *parameter,
                            gpointer        user_data);

void settings_dialog_cb    (GSimpleAction  *simple,
                            GVariant       *parameter,
                            gpointer        user_data);

void shortcuts_window_cb   (GSimpleAction  *simple,
                            GVariant       *parameter,
                            gpointer        user_data);

G_END_DECLS
