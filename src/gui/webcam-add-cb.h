#pragma once

#include "gtk-compat.h"

G_BEGIN_DECLS

void webcam_add_cb         (GSimpleAction  *simple,
                            GVariant       *parameter,
                            gpointer        user_data);

void webcam_add_cb_shortcut (GtkWidget      *w,
                             gpointer       user_data);

G_END_DECLS

