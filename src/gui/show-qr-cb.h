#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

void show_qr_cb          (GtkMenuItem *menu_item,
                          gpointer     user_data);

void show_qr_cb_shortcut (GtkWidget   *w,
                          gpointer     user_data);

G_END_DECLS
