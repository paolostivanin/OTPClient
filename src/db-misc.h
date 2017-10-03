#pragma once

#define SUCCESS (gpointer) 0
#define GENERIC_ERROR (gpointer) 1
#define FILE_CORRUPTED (gpointer) 2
#define SECURE_MEMORY_ALLOC_ERR (gpointer) 3
#define KEY_DERIV_ERR (gpointer) 4
#define FILE_TOO_BIG (gpointer) 5

#define DB_UPDATE_OK 20
#define DB_UPDATE_FAILED 21

#define IV_SIZE 16
#define KDF_ITERATIONS 100000
#define KDF_SALT_SIZE 32
#define TAG_SIZE 16

#define MAX_FILE_SIZE 67108864  // 64 MiB should be more than enough for such content.

typedef struct _header_data {
    guint8 iv[IV_SIZE];
    guint8 salt[KDF_SALT_SIZE];
} HeaderData;

typedef struct _db_data {
    JsonNode *json_data;
    gchar *key;
    GList *objects_hash;
    GHashTable *data_to_add;    // {account_name: secret,digits; account_name2: secret2,digits2 ecc}
} DatabaseData;

gpointer load_db (DatabaseData *db_data);

void reload_db (DatabaseData *kf_data);

gpointer encrypt_db (const gchar *in_memory_json, const gchar *password);

gchar *decrypt_db (const gchar *path, const gchar *password);

gint update_db (DatabaseData *data);