#pragma once

#include <gio/gio.h>
#include "common.h"

G_BEGIN_DECLS

#define GENERIC_ERROR           (gpointer)1
#define TAG_MISMATCH            (gpointer)2
#define SECURE_MEMORY_ALLOC_ERR (gpointer)3
#define KEY_DERIV_ERR           (gpointer)4

#define IV_SIZE                 16
#define KDF_ITERATIONS          100000
#define KDF_SALT_SIZE           32
#define TAG_SIZE                16


typedef struct db_header_data_t {
    guint8 iv[IV_SIZE];
    guint8 salt[KDF_SALT_SIZE];
} DbHeaderData;


typedef struct db_data_t {
    gchar *db_path;

    gchar *key;

    json_t *json_data;

    GSList *objects_hash;

    GSList *data_to_add;

    gint32 max_file_size_from_memlock;

    gchar *last_hotp;
    GDateTime *last_hotp_update;

    gboolean key_stored;
} DatabaseData;


void    load_db            (DatabaseData   *db_data,
                            GError        **error);

void    update_db          (DatabaseData  *db_data,
                            GError       **err);

void    reload_db          (DatabaseData  *db_data,
                            GError       **err);

guchar *get_db_derived_key (const gchar   *pwd,
                            DbHeaderData  *header_data);

void    add_otps_to_db     (GSList       *otps,
                            DatabaseData *db_data);

gint    check_duplicate    (gconstpointer   data,
                            gconstpointer   user_data);

void    cleanup_db_gfile   (GFile         *file,
                            gpointer       stream,
                            GError        *err);

void    free_db_resources  (gcry_cipher_hd_t hd,
                            guchar        *derived_key,
                            guchar        *enc_buf,
                            gchar         *dec_buf,
                            DbHeaderData  *header_data);

G_END_DECLS