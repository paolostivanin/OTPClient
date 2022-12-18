#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

void     edit_row_cb          (GSimpleAction  *simple,
                               GVariant       *parameter,
                               gpointer        user_data);

void     edit_row_cb_shortcut (GtkWidget      *w,
                               gpointer        user_data);

G_END_DECLS