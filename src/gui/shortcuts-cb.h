#pragma once

#include "gtk-compat.h"

G_BEGIN_DECLS

void shortcuts_window_cb  (GSimpleAction  *simple,
                           GVariant       *parameter,
                           gpointer        user_data);

void show_kbs_cb_shortcut (GtkWidget      *w,
                           gpointer        user_data);

G_END_DECLS

