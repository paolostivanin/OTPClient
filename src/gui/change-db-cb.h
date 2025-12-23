#pragma once

#include "gtk-compat.h"

G_BEGIN_DECLS

int  change_db             (AppData *app_data);

void change_db_cb          (GSimpleAction *simple,
                            GVariant      *parameter,
                            gpointer       user_data);

void change_db_cb_shortcut (GtkWidget     *w,
                            gpointer       user_data);

G_END_DECLS
