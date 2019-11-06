#pragma once

#include <glib.h>
#include "../db-misc.h"

G_BEGIN_DECLS

void show_token         (DatabaseData *db_data,
                         const gchar  *account,
                         const gchar  *issuer,
                         gboolean      match_exactly,
                         gboolean      show_next_token);

void list_all_acc_iss   (DatabaseData  *db_data);

G_END_DECLS