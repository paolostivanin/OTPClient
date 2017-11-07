#pragma once

#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define GENERIC_ERROR           (gpointer) 1
#define TAG_MISMATCH            (gpointer) 2
#define SECURE_MEMORY_ALLOC_ERR (gpointer) 3
#define KEY_DERIV_ERR           (gpointer) 4

#define MISSING_FILE_CODE       10
#define BAD_TAG                 11

#define IV_SIZE                 16
#define KDF_ITERATIONS          100000
#define KDF_SALT_SIZE           32
#define TAG_SIZE                16

#define MAX_FILE_SIZE           1048576  // 1 MiB should be more than enough for such content.


typedef struct _header_data {
    guint8 iv[IV_SIZE];
    guint8 salt[KDF_SALT_SIZE];
} HeaderData;

typedef struct _db_data {
    JsonNode *json_data;

    gchar *key;

    GSList *objects_hash;

    GSList *data_to_add;
} DatabaseData;


void load_db            (DatabaseData   *db_data,
                         GError        **error);

void reload_db          (DatabaseData   *db_data,
                         GError        **err);

void update_db          (DatabaseData   *data);

gint check_duplicate    (gconstpointer data,
                         gconstpointer user_data);

G_END_DECLS