#pragma once

#include <gio/gio.h>
#include "common.h"

G_BEGIN_DECLS

// Old parameter used to derive the db's password (v1)
#define KDF_ITERATIONS          100000

// Header data
#define DB_HEADER_NAME         "OTPClient"
#define DB_HEADER_NAME_LEN      9
#define DB_VERSION              3
#define IV_SIZE                 16
#define KDF_SALT_SIZE           32
#define DB_V3_HEADER_SIZE       (DB_HEADER_NAME_LEN + 4 + IV_SIZE + KDF_SALT_SIZE + 4 + 4 + 4)

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
#define ARGON2ID_MAX_ITER          64
#define ARGON2ID_MIN_MC          8192   // 8 MiB
#define ARGON2ID_MAX_MC       1048576   // 1 GiB
#define ARGON2ID_MIN_PARAL          1
#define ARGON2ID_MAX_PARAL         16


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
    gint ref_count;

    gchar *db_path;

    gchar *key;

    json_t *in_memory_json_data;

    json_t *committed_json_data;

    /* Tokens that failed validation on load (out-of-range digits/period, invalid
     * secret, unsupported algo, ...). Kept aside instead of bricking the whole
     * database (issues #458/#462/#464): the valid tokens load, these are preserved
     * verbatim and re-merged into the file on every save, and the UI surfaces them
     * for the user to repair or delete. NULL until load runs. */
    json_t *quarantined_tokens;

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

    // Cached Argon2id-derived key + the salt and password-hash that produced
    // it. The KDF costs ~150 ms per call, so reusing the derived key across
    // saves makes edits feel instantaneous. Reusing the salt within one
    // database is safe: the per-save random IV provides AES-GCM nonce
    // uniqueness, and the salt's job (defeating rainbow tables) is unaffected
    // by reuse against the same password. cached_pwd_hash is SHA-256 of the
    // password bytes and is checked alongside the salt on lookup so a cache
    // entry from one password never gets returned to a different one. Cache
    // must be invalidated on password change via db_invalidate_kdf_cache().
    guchar *cached_derived_key;     // gcry secure memory; ARGON2ID_KEYLEN bytes
    guint8 cached_salt[KDF_SALT_SIZE];
    guint8 cached_pwd_hash[32];     // SHA-256(db_data->key) when cache populated
    gboolean has_cached_key;

    guint8 loaded_file_digest[32];
    gboolean has_loaded_file_digest;
} DatabaseData;

typedef gboolean (*DbMutationFunc) (json_t  *candidate,
                                    gpointer user_data,
                                    GError **err);

typedef struct {
    guint added;
    guint skipped_duplicates;
    guint skipped_invalid;
} OtpImportReport;

typedef struct {
    gchar *code; /* Secure memory; owned by the result array. */
    guint64 next_counter;
} DbHotpResult;

/* Generates in index order and commits all counter advances atomically.
 * Returns an owned array of DbHotpResult, or NULL with no advances on failure.
 * Nothing may deliver a generated code before this operation succeeds. */
GPtrArray *db_generate_hotp (DatabaseData *db_data, const gsize *indices,
                             gsize n_indices, GError **err);


DatabaseData *database_data_new  (const gchar  *db_path,
                                  gint32        max_file_size_from_memlock);

DatabaseData *database_data_ref  (DatabaseData *db_data);

void          database_data_unref (DatabaseData *db_data);

void          database_data_free (DatabaseData *db_data);

/* Number of tokens set aside on load because they failed validation (preserved
 * in the file and surfaced to the user for repair). 0 in the normal case. */
guint         db_get_quarantined_count (DatabaseData *db_data);

void    load_db            (DatabaseData   *db_data,
                            GError        **error);

void    update_db          (DatabaseData  *db_data,
                            GError       **err);

gboolean db_transaction    (DatabaseData  *db_data,
                            DbMutationFunc mutation,
                            gpointer       user_data,
                            GError       **err);

gboolean db_change_password (DatabaseData  *db_data,
                             const gchar   *new_password,
                             GError       **err);

gboolean db_update_kdf_params (DatabaseData *db_data,
                               gint32        iter,
                               gint32        memcost,
                               gint32        parallelism,
                               GError      **err);

gboolean db_import_otps    (DatabaseData      *db_data,
                            GSList            *otps,
                            OtpImportReport   *report,
                            GError          **err);

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
void    database_data_purge_secrets (DatabaseData *db_data);

/* Copies an encrypted database file to dst_path with 0600 perms. The source
 * is not followed if it is a symlink, and the destination is forced 0600
 * regardless of any pre-existing perms - same hardening used for the .bak
 * sibling created on every successful save. Returns NULL on success, or a
 * newly-allocated, translated error message the caller frees with g_free. */
gchar  *db_copy_to              (const gchar  *src_path,
                                 const gchar  *dst_path);

#ifdef OTPCLIENT_TESTING
/* Which lock attempts should fail, and how. The UNSUPPORTED_* modes report
 * ENOSYS from flock: BESIDE_DB models the case that actually happens, a
 * database reached through the Flatpak document portal, where the lock file
 * next to it cannot be locked while the fallback in the user data dir can;
 * EVERYWHERE models an NFS home with no lock support at all.
 *
 * The OPEN_FAILS_* modes fail earlier, at creating the lock file: a read-only
 * mount, a full or over-quota filesystem, a directory we may not write. That
 * is a different code path from ENOSYS and it used to abort the save outright,
 * so both locations need their own coverage. */
typedef enum {
    DB_TEST_LOCK_SUPPORTED = 0,
    DB_TEST_LOCK_UNSUPPORTED_BESIDE_DB,
    DB_TEST_LOCK_UNSUPPORTED_EVERYWHERE,
    DB_TEST_LOCK_OPEN_FAILS_BESIDE_DB,
    DB_TEST_LOCK_OPEN_FAILS_EVERYWHERE
} DbTestLockMode;

void    db_test_set_fail_encrypt      (gboolean fail);
void    db_test_set_fail_atomic_write (gboolean fail);
/* Force the quarantined-token merge in encrypt_db to fail as if the append
 * hit OOM: the save must abort with an error instead of silently dropping the
 * token from the serialized database. */
void    db_test_set_fail_quarantine_append (gboolean fail);
/* Force the post-commit digest baseline installation to fail, exercising the
 * explicit no-baseline fallback: the save must still succeed and the next one
 * must not be blocked by a stale "Database changed on disk" guard. */
void    db_test_set_fail_committed_digest  (gboolean fail);
void    db_test_set_lock_mode         (DbTestLockMode mode);

/* Let `n_successes` decrypts through and fail every one after that, or -1 to
 * stop injecting. Counted rather than a flag because load_db decrypts twice on
 * the migration path and only the second failure is interesting. Combined with
 * db_test_set_force_migration these reach the one path where a failed decrypt
 * used to leave in_memory_json_data pointing at freed memory for
 * database_data_purge_secrets to decref a second time. */
void    db_test_fail_decrypt_after    (gint n_successes);
void    db_test_set_force_migration   (gboolean force);
#endif

G_END_DECLS
