#pragma once

G_BEGIN_DECLS

#include "db-misc.h"

void traverse_liststore (GtkListStore   *list_store,
                         DatabaseData   *db_data);

void set_otp            (GtkListStore   *list_store,
                         GtkTreeIter     iter,
                         DatabaseData   *db_data);

G_END_DECLS