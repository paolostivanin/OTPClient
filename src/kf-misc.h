#pragma once

#define FILE_EMPTY (gpointer) 1
#define GENERIC_ERROR (gpointer) 2
#define FILE_CORRUPTED (gpointer) 3
#define SECURE_MEMORY_ALLOC_ERR (gpointer) 4
#define KEY_DERIV_ERR (gpointer) 5

#define IV_SIZE 16
#define KDF_ITERATIONS 100000
#define KDF_SALT_SIZE 32
#define TAG_SIZE 16

typedef struct _header_data {
    guint8 iv[IV_SIZE];
    guint8 salt[KDF_SALT_SIZE];
} HeaderData;

gboolean create_kf (const gchar *path);

guchar *get_derived_key (const gchar *pwd, HeaderData *header_data);

goffset get_file_size (const gchar *path);

gpointer encrypt_kf (const gchar *path, const gchar *password);

gchar *decrypt_kf (const gchar *path, const gchar *password);