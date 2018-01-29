#pragma once

#include <jansson.h>

G_BEGIN_DECLS

#define GENERIC_ERROR           (gpointer) 1
#define TAG_MISMATCH            (gpointer) 2
#define SECURE_MEMORY_ALLOC_ERR (gpointer) 3
#define KEY_DERIV_ERR           (gpointer) 4

#define IV_SIZE                 16
#define KDF_ITERATIONS          100000
#define KDF_SALT_SIZE           32
#define TAG_SIZE                16

typedef struct _header_data {
    guint8 iv[IV_SIZE];
    guint8 salt[KDF_SALT_SIZE];
} HeaderData;

typedef struct _db_data {
    gchar *db_path;

    gchar *key;

    json_t *json_data;

    GSList *objects_hash;

    GSList *data_to_add;

    gint32 max_file_size_from_memlock;

    gchar *last_hotp;
    GDateTime *last_hotp_update;
} DatabaseData;


void load_db                (DatabaseData   *db_data,
                             GError        **error);

void update_and_reload_db   (DatabaseData   *db_data,
                             GtkListStore   *list_store,
                             gboolean        regenerate_model,
                             GError        **err);

gint check_duplicate        (gconstpointer data,
                             gconstpointer user_data);

G_END_DECLS