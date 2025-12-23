#pragma once

#include "gtk-compat.h"

G_BEGIN_DECLS

void change_password_cb     (GSimpleAction *simple,
                             GVariant      *parameter,
                             gpointer       user_data);

void change_pwd_cb_shortcut (GtkWidget     *w,
                             gpointer       user_data);

G_END_DECLS