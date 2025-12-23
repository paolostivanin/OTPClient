#pragma once

#include "gtk-compat.h"
#include "data.h"

G_BEGIN_DECLS

gboolean new_db     (AppData       *app_data);

void     new_db_cb  (GSimpleAction *simple,
                     GVariant      *parameter,
                     gpointer       user_data);

G_END_DECLS
