#pragma once

G_BEGIN_DECLS

#include "db-misc.h"

gboolean traverse_liststore (GtkTreeModel   *model,
                             GtkTreePath    *path,
                             GtkTreeIter    *iter,
                             gpointer        user_data);

void set_otp                (GtkListStore   *list_store,
                             GtkTreeIter     iter,
                             DatabaseData   *db_data);

G_END_DECLS