#pragma once

#include <jansson.h>
#include "../common/db-common.h"
#include "data.h"

G_BEGIN_DECLS

void load_new_db      (AppData        *app_data,
                       GError        **err);

void regenerate_model (AppData        *app_data);

void write_db_to_disk (DatabaseData   *db_data,
                       GError        **err);

gint check_duplicate  (gconstpointer   data,
                       gconstpointer   user_data);

G_END_DECLS
