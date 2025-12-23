#pragma once

#include "gtk-compat.h"

G_BEGIN_DECLS

void edit_row_cb          (GtkWidget *menu_item,
                           gpointer   user_data);

void edit_row_cb_shortcut (GtkWidget   *w,
                           gpointer     user_data);

G_END_DECLS
