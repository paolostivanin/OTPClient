#pragma once

#define GENERIC_ERROR (gpointer) 1
#define FILE_CORRUPTED (gpointer) 2
#define SECURE_MEMORY_ALLOC_ERR (gpointer) 3
#define KEY_DERIV_ERR (gpointer) 4
#define FILE_TOO_BIG (gpointer) 5

#define KF_UPDATE_OK 20
#define KF_UPDATE_FAILED 21

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
    GHashTable *data_to_add;    // {account_name: secret,digits; account_name2: secret2,digits2 ecc}
} UpdateData;

gchar *load_kf (const gchar *plain_key);

void reload_kf (UpdateData *kf_data);

gpointer encrypt_kf (const gchar *path, const gchar *password);

gchar *decrypt_kf (const gchar *path, const gchar *password);

gint update_kf (UpdateData *data, gboolean is_add);