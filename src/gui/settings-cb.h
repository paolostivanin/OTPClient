#pragma once

#include "gtk-compat.h"

G_BEGIN_DECLS

void settings_dialog_cb        (GSimpleAction  *simple,
                                GVariant       *parameter,
                                gpointer        user_data);

void show_settings_cb_shortcut (GtkWidget      *w,
                                gpointer        user_data);

G_END_DECLS