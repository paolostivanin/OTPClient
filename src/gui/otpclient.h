#pragma once

#include "treeview.h"

G_BEGIN_DECLS

#define HOTP_RATE_LIMIT_IN_SEC  3

#define NOTIFICATION_ID "otp-copied"

void     add_qr_from_file      (GSimpleAction  *simple,
                                GVariant       *parameter,
                                gpointer        user_data);

void     add_qr_from_clipboard (GSimpleAction *simple,
                                GVariant      *parameter,
                                gpointer       user_data);

void     about_diag_cb         (GSimpleAction  *simple,
                                GVariant       *parameter,
                                gpointer        user_data);

void     destroy_cb            (GtkWidget      *window,
                                gpointer        user_data);

G_END_DECLS
