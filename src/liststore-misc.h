#pragma once

G_BEGIN_DECLS

#include "db-misc.h"

gboolean traverse_liststore (gpointer        user_data);

void     set_otp            (GtkListStore   *list_store,
                             GtkTreeIter     iter,
                             AppData        *app_data);

G_END_DECLS
