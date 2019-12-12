#pragma once

#include <jansson.h>
#include "data.h"

G_BEGIN_DECLS

#define GENERIC_ERROR           (gpointer)1
#define TAG_MISMATCH            (gpointer)2
#define SECURE_MEMORY_ALLOC_ERR (gpointer)3
#define KEY_DERIV_ERR           (gpointer)4

#define IV_SIZE                 16
#define KDF_ITERATIONS          100000
#define KDF_SALT_SIZE           32
#define TAG_SIZE                16

void load_db                (DatabaseData   *db_data,
                             GError        **error);

void update_and_reload_db   (AppData        *app_data,
                             DatabaseData   *db_data,
                             gboolean        regenerate_model,
                             GError        **err);

gint check_duplicate        (gconstpointer data,
                             gconstpointer user_data);

G_END_DECLS