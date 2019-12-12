#pragma once

#include "treeview.h"

G_BEGIN_DECLS

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

void add_qr_from_file      (GSimpleAction  *simple,
                            GVariant       *parameter,
                            gpointer        user_data);

void add_qr_from_clipboard (GSimpleAction *simple,
                            GVariant      *parameter,
                            gpointer       user_data);

void edit_selected_row_cb  (GSimpleAction  *simple,
                            GVariant       *parameter,
                            gpointer        user_data);

void settings_dialog_cb    (GSimpleAction  *simple,
                            GVariant       *parameter,
                            gpointer        user_data);

void shortcuts_window_cb   (GSimpleAction  *simple,
                            GVariant       *parameter,
                            gpointer        user_data);

void destroy_cb            (GtkWidget      *window,
                            gpointer        user_data);

G_END_DECLS
