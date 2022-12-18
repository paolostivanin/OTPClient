#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

void change_db_cb          (GSimpleAction *simple,
                            GVariant      *parameter,
                            gpointer       user_data);

void change_db_cb_shortcut (GtkWidget     *w,
                            gpointer       user_data);

G_END_DECLS
