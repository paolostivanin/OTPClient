#pragma once

#include <gio/gio.h>
#include "common.h"

G_BEGIN_DECLS

// Old parameter used to derive the db's password (v1)
#define KDF_ITERATIONS          100000

// Header data
#define DB_HEADER_NAME         "OTPClient"
#define DB_HEADER_NAME_LEN      9
#define DB_VERSION              2
#define IV_SIZE                 16
#define KDF_SALT_SIZE           32

// Parameter used by the encryption routine (+IV from header data)
#define TAG_SIZE                16

// Parameters used to derive the db's password (v2)
#define ARGON2ID_TAGLEN         32
#define ARGON2ID_ITER           4
#define ARGON2ID_MEMCOST        131072 // (128 MiB)
#define ARGON2ID_PARALLELISM    4
#define ARGON2ID_KEYLEN         32


typedef struct db_header_data_v1_t {
    guint8 iv[IV_SIZE];
    guint8 salt[KDF_SALT_SIZE];
} DbHeaderData_v1;


typedef struct db_header_data_v2_t {
    gchar header_name[DB_HEADER_NAME_LEN];
    gint32 db_version;
    guint8 iv[IV_SIZE];
    guint8 salt[KDF_SALT_SIZE];
} DbHeaderData_v2;


typedef struct db_data_t {
    gchar *db_path;

    gchar *key;

    json_t *in_memory_json_data;

    GSList *objects_hash;

    GSList *data_to_add;

    gint32 max_file_size_from_memlock;

    gchar *last_hotp;
    GDateTime *last_hotp_update;

    gboolean key_stored;

    gint32 current_db_version;
} DatabaseData;


void    load_db            (DatabaseData   *db_data,
                            GError        **error);

void    update_db          (DatabaseData  *db_data,
                            GError       **err);

void    reload_db          (DatabaseData  *db_data,
                            GError       **err);

void    add_otps_to_db     (GSList       *otps,
                            DatabaseData *db_data);

gint    check_duplicate    (gconstpointer   data,
                            gconstpointer   user_data);

G_END_DECLS
