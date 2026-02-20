#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

void save_sort_order  (GtkTreeView *tree_view);

void save_window_size (gint         width,
                       gint         height);

G_END_DECLS
