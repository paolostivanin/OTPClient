#pragma once

#include <glib.h>
#include "main.h"

G_BEGIN_DECLS

gboolean show_token     (DatabaseData *db_data,
                         const gchar  *account,
                         const gchar  *issuer,
                         gboolean      match_exactly,
                         gboolean      show_next_token,
                         OutputFormat  format);

void list_all_acc_iss   (DatabaseData *db_data,
                         OutputFormat  format);

void csv_append_field   (GString      *out,
                         const gchar  *value);

G_END_DECLS
