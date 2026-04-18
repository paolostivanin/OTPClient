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
#define ARGON2ID_TAGLEN            32
#define ARGON2ID_KEYLEN            32
#define ARGON2ID_DEFAULT_ITER       4
#define ARGON2ID_DEFAULT_MC    131072  //128 MiB
#define ARGON2ID_DEFAULT_PARAL      4

// Bounds enforced when reading Argon2id parameters from a (potentially-tampered) DB header.
// Below these floors the KDF would be cryptographically weak; above the ceilings
// the open path is a denial-of-service vector (excessive memory / CPU).
#define ARGON2ID_MIN_ITER           1
#define ARGON2ID_MAX_ITER         100
#define ARGON2ID_MIN_MC          8192   // 8 MiB
#define ARGON2ID_MAX_MC       4194304   // 4 GiB
#define ARGON2ID_MIN_PARAL          1
#define ARGON2ID_MAX_PARAL         64


typedef struct db_header_data_v1_t {
    guint8 iv[IV_SIZE];
    guint8 salt[KDF_SALT_SIZE];
} DbHeaderData_v1;


typedef struct db_header_data_v2_t {
    gchar header_name[DB_HEADER_NAME_LEN];
    gint32 db_version;
    guint8 iv[IV_SIZE];
    guint8 salt[KDF_SALT_SIZE];
    gint32 argon2id_iter;
    gint32 argon2id_memcost;
    gint32 argon2id_parallelism;
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

    gint32 argon2id_iter;
    gint32 argon2id_memcost;
    gint32 argon2id_parallelism;

    // Set TRUE when decrypt_db succeeded only with the legacy g_utf8_strlen
    // password byte length. The next encrypt_db will silently re-encrypt with
    // the corrected strlen length, migrating the file in place.
    gboolean needs_legacy_kdf_migration;

    // Cached Argon2id-derived key + the salt that produced it. The KDF costs
    // ~150 ms per call, so reusing the derived key across saves makes edits
    // feel instantaneous. Reusing the salt within one database is safe: the
    // per-save random IV provides AES-GCM nonce uniqueness, and the salt's
    // job (defeating rainbow tables) is unaffected by reuse against the same
    // password. Cache must be invalidated on password change via
    // db_invalidate_kdf_cache().
    guchar *cached_derived_key;     // gcry secure memory; ARGON2ID_KEYLEN bytes
    guint8 cached_salt[KDF_SALT_SIZE];
    gboolean has_cached_key;
} DatabaseData;


void    load_db            (DatabaseData   *db_data,
                            GError        **error);

void    update_db          (DatabaseData  *db_data,
                            GError       **err);

void    reload_db          (DatabaseData  *db_data,
                            GError       **err);

void    add_otps_to_db     (GSList       *otps,
                            DatabaseData *db_data);

/* Same as add_otps_to_db but reports how many entries were appended versus
 * skipped as duplicates. Either out-param may be NULL. */
void    add_otps_to_db_ex  (GSList       *otps,
                            DatabaseData *db_data,
                            guint        *added_out,
                            guint        *skipped_out);

gint    check_duplicate    (gconstpointer   data,
                            gconstpointer   user_data);

void    db_invalidate_kdf_cache (DatabaseData *db_data);

G_END_DECLS
