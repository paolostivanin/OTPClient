#pragma once

#define FILE_EMPTY (gpointer) 1
#define GENERIC_ERROR (gpointer) 2
#define FILE_CORRUPTED (gpointer) 3
#define SECURE_MEMORY_ALLOC_ERR (gpointer) 4
#define KEY_DERIV_ERR (gpointer) 5
#define FILE_TOO_BIG (gpointer) 6

#define IV_SIZE 16
#define KDF_ITERATIONS 100000
#define KDF_SALT_SIZE 32
#define TAG_SIZE 16

#define MAX_FILE_SIZE 64*1024*1024  // 64 MB should be more than enough for such content.

typedef struct _header_data {
    guint8 iv[IV_SIZE];
    guint8 salt[KDF_SALT_SIZE];
} HeaderData;

typedef struct _update_data {
    gchar *in_memory_kf;
    gchar *key;
    GHashTable *data_to_add;    // {account: secret, accoun2: secret2, ecc}
} UpdateData;

gchar *load_kf (const gchar *plain_key);

gpointer encrypt_kf (const gchar *path, const gchar *password);

gchar *decrypt_kf (const gchar *path, const gchar *password);