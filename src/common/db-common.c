#define _DEFAULT_SOURCE
#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gcrypt.h>
#include <jansson.h>
#include <glib/gstdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/file.h>
#include "gquarks.h"
#include "db-common.h"
#include "file-size.h"
#include "otp-validation.h"


typedef struct {
    int fd;
    gchar *path;
} DbLock;

#ifdef OTPCLIENT_TESTING
static gboolean test_fail_encrypt = FALSE;
static gboolean test_fail_atomic_write = FALSE;
static gboolean test_unsupported_lock = FALSE;

void
db_test_set_fail_encrypt (gboolean fail)
{
    test_fail_encrypt = fail;
}

void
db_test_set_fail_atomic_write (gboolean fail)
{
    test_fail_atomic_write = fail;
}

void
db_test_set_unsupported_lock (gboolean unsupported)
{
    test_unsupported_lock = unsupported;
}
#endif

static gint32    get_db_version     (const gchar      *db_path,
                                     GError          **err);

static gboolean  lock_db            (const gchar      *db_path,
                                     DbLock           *lock,
                                     GError          **err);

static void      unlock_db          (DbLock           *lock);

static guchar   *get_db_derived_key (DatabaseData     *db_data,
                                     gint32            db_version,
                                     const guint8     *salt,
                                     gboolean          use_legacy_length,
                                     GError          **err);

static gchar    *try_decrypt_v2     (DatabaseData     *db_data,
                                     DbHeaderData_v2  *header_data,
                                     guchar           *enc_buf,
                                     gsize             enc_buf_size,
                                     const guchar     *tag,
                                     gboolean          use_legacy_length,
                                     GError          **err);

static gchar    *try_decrypt_v3     (DatabaseData     *db_data,
                                     const guint8     *header_data,
                                     gsize             header_data_size,
                                     const guint8     *salt,
                                     const guint8     *iv,
                                     guchar           *enc_buf,
                                     gsize             enc_buf_size,
                                     const guchar     *tag,
                                     gboolean          use_legacy_length,
                                     GError          **err);

static gchar    *decrypt_db         (DatabaseData     *db_data,
                                     gsize            *dec_len,
                                     GError          **err);

static gboolean  encrypt_db         (DatabaseData     *db_data,
                                     json_t           *json_data,
                                     GError          **err);

static void      add_to_json        (gpointer          list_elem,
                                     gpointer          json_array);

static gboolean  backup_db          (const gchar      *path,
                                     GError          **err);

static void      cleanup_db_gfile   (GFile            *file,
                                     gpointer          stream,
                                     GError           *err);

static void      free_db_resources  (gcry_cipher_hd_t  hd,
                                     guchar           *derived_key,
                                     guchar           *enc_buf,
                                     gchar            *dec_buf,
                                     DbHeaderData_v1  *header_data_v1,
                                     DbHeaderData_v2  *header_data_v2);

static gboolean  compute_file_digest (const gchar      *path,
                                      guint8            digest[32],
                                      GError          **err);

static gboolean  loaded_file_digest_matches (DatabaseData *db_data,
                                             GError      **err);

static void      rebuild_objects_hash (DatabaseData *db_data);

static void      refresh_committed_snapshot (DatabaseData *db_data);

static void      restore_live_from_committed (DatabaseData *db_data);

static gboolean  partition_valid_tokens (DatabaseData     *db_data,
                                         GError          **err);


void
db_invalidate_kdf_cache (DatabaseData *db_data)
{
    if (db_data == NULL)
        return;
    if (db_data->cached_derived_key != NULL) {
        gcry_free (db_data->cached_derived_key);
        db_data->cached_derived_key = NULL;
    }
    explicit_bzero (db_data->cached_salt, KDF_SALT_SIZE);
    explicit_bzero (db_data->cached_pwd_hash, sizeof (db_data->cached_pwd_hash));
    db_data->has_cached_key = FALSE;
}

void
database_data_purge_secrets (DatabaseData *db_data)
{
    if (db_data == NULL)
        return;

    db_invalidate_kdf_cache (db_data);

    if (db_data->key != NULL) {
        explicit_bzero (db_data->key, strlen (db_data->key));
        gcry_free (db_data->key);
        db_data->key = NULL;
    }

    if (db_data->in_memory_json_data != NULL) {
        json_decref (db_data->in_memory_json_data);
        db_data->in_memory_json_data = NULL;
    }
    if (db_data->committed_json_data != NULL) {
        json_decref (db_data->committed_json_data);
        db_data->committed_json_data = NULL;
    }
    if (db_data->quarantined_tokens != NULL) {
        json_decref (db_data->quarantined_tokens);
        db_data->quarantined_tokens = NULL;
    }

    g_slist_free_full (db_data->data_to_add, (GDestroyNotify) json_decref);
    db_data->data_to_add = NULL;
    g_slist_free_full (db_data->objects_hash, g_free);
    db_data->objects_hash = NULL;

    if (db_data->last_hotp != NULL) {
        explicit_bzero (db_data->last_hotp, strlen (db_data->last_hotp));
        g_free (db_data->last_hotp);
        db_data->last_hotp = NULL;
    }
    g_clear_pointer (&db_data->last_hotp_update, g_date_time_unref);

    explicit_bzero (db_data->loaded_file_digest,
                    sizeof (db_data->loaded_file_digest));
    db_data->has_loaded_file_digest = FALSE;
    db_data->needs_legacy_kdf_migration = FALSE;
}


static guint32
read_be32 (const guint8 *p)
{
    return ((guint32) p[0] << 24) |
           ((guint32) p[1] << 16) |
           ((guint32) p[2] << 8) |
           (guint32) p[3];
}


static void
write_be32 (guint8 *p,
            guint32 v)
{
    p[0] = (guint8) ((v >> 24) & 0xff);
    p[1] = (guint8) ((v >> 16) & 0xff);
    p[2] = (guint8) ((v >> 8) & 0xff);
    p[3] = (guint8) (v & 0xff);
}


static gboolean
write_all_fd (int           fd,
              const void   *buf,
              gsize         len,
              GError      **err)
{
    const guint8 *p = buf;
    gsize written = 0;
    while (written < len) {
        ssize_t n = write (fd, p + written, len - written);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                         "Failed to write database file: %s", g_strerror (errno));
            return FALSE;
        }
        if (n == 0) {
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                         "Short write while writing database file.");
            return FALSE;
        }
        written += (gsize) n;
    }
    return TRUE;
}


static gboolean
fsync_parent_dir (const gchar *path)
{
    g_autofree gchar *dir = g_path_get_dirname (path);
    int dir_fd = g_open (dir, O_RDONLY | O_CLOEXEC, 0);
    if (dir_fd < 0)
        return FALSE;
    gboolean ok = (fsync (dir_fd) == 0);
    close (dir_fd);
    return ok;
}


static gboolean
atomic_write_database (const gchar  *path,
                       const guint8 *header,
                       gsize         header_len,
                       const guint8 *enc_buf,
                       gsize         enc_len,
                       const guint8  tag[TAG_SIZE],
                       GError      **err)
{
#ifdef OTPCLIENT_TESTING
    if (test_fail_atomic_write) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Injected atomic write failure.");
        return FALSE;
    }
#endif

    g_autofree gchar *dir = g_path_get_dirname (path);
    g_autofree gchar *base = g_path_get_basename (path);
    g_autofree gchar *tmpl = g_strdup_printf ("%s/.%s.tmp.XXXXXX", dir, base);

    int fd = g_mkstemp_full (tmpl, O_WRONLY | O_CLOEXEC, 0600);
    if (fd < 0) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Failed to create temporary database file: %s", g_strerror (errno));
        return FALSE;
    }

    gboolean ok = write_all_fd (fd, header, header_len, err) &&
                  write_all_fd (fd, enc_buf, enc_len, err) &&
                  write_all_fd (fd, tag, TAG_SIZE, err);
    if (ok && fsync (fd) != 0) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Failed to fsync temporary database file: %s", g_strerror (errno));
        ok = FALSE;
    }
    if (close (fd) != 0 && ok) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Failed to close temporary database file: %s", g_strerror (errno));
        ok = FALSE;
    }

    if (!ok) {
        g_unlink (tmpl);
        return FALSE;
    }

    if (g_rename (tmpl, path) != 0) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Failed to replace database file: %s", g_strerror (errno));
        g_unlink (tmpl);
        return FALSE;
    }

    if (!fsync_parent_dir (path)) {
        g_warning ("Failed to fsync parent directory for %s: %s", path, g_strerror (errno));
    }
    return TRUE;
}


/* Wrapper around flock() so tests can force the "filesystem does not support
 * locking" path (see db_test_set_unsupported_lock). */
static int
db_try_flock (int fd)
{
#ifdef OTPCLIENT_TESTING
    if (test_unsupported_lock) {
        errno = ENOSYS;
        return -1;
    }
#endif
    return flock (fd, LOCK_EX | LOCK_NB);
}


static gboolean
lock_db (const gchar *db_path,
         DbLock      *lock,
         GError     **err)
{
    g_return_val_if_fail (lock != NULL, FALSE);

    lock->fd = -1;
    lock->path = g_strconcat (db_path, ".lock", NULL);
    lock->fd = g_open (lock->path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock->fd < 0) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Failed to open database lock '%s': %s", lock->path, g_strerror (errno));
        g_clear_pointer (&lock->path, g_free);
        return FALSE;
    }

    gint64 deadline = g_get_monotonic_time () + (2 * G_USEC_PER_SEC);
    while (db_try_flock (lock->fd) != 0) {
        if (errno == ENOSYS || errno == EOPNOTSUPP) {
            /* Some filesystems do not implement POSIX locks and fail with
             * ENOSYS/EOPNOTSUPP, notably the Flatpak XDG document-portal FUSE
             * mount (/run/user/<uid>/doc/) and some NFS/SMB setups. The lock is
             * a best-effort guard against a second OTPClient instance writing
             * concurrently, not a correctness requirement, so continue without
             * it rather than making the database impossible to open (issue #466).
             * Leave lock->fd open; unlock_db () cleans it up. Warn once so we do
             * not spam on every write. */
            static gboolean warned = FALSE;
            if (!warned) {
                g_warning ("Database lock not supported on this filesystem '%s'; "
                           "continuing without a lock: %s", lock->path, g_strerror (errno));
                warned = TRUE;
            }
            return TRUE;
        }
        if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                         "Failed to acquire database lock '%s': %s", lock->path, g_strerror (errno));
            unlock_db (lock);
            return FALSE;
        }
        if (g_get_monotonic_time () >= deadline) {
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                         "Database is busy. Another OTPClient process is writing it; retry shortly.");
            unlock_db (lock);
            return FALSE;
        }
        g_usleep (50 * 1000);
    }
    return TRUE;
}


static void
unlock_db (DbLock *lock)
{
    if (lock == NULL)
        return;
    if (lock->fd >= 0) {
        flock (lock->fd, LOCK_UN);
        close (lock->fd);
        lock->fd = -1;
    }
    g_clear_pointer (&lock->path, g_free);
}


DatabaseData *
database_data_new (const gchar *db_path,
                   gint32       max_file_size_from_memlock)
{
    DatabaseData *db_data = g_new0 (DatabaseData, 1);
    g_atomic_int_set (&db_data->ref_count, 1);
    db_data->db_path = g_strdup (db_path);
    db_data->max_file_size_from_memlock = max_file_size_from_memlock;
    return db_data;
}


DatabaseData *
database_data_ref (DatabaseData *db_data)
{
    g_return_val_if_fail (db_data != NULL, NULL);

    if (g_atomic_int_get (&db_data->ref_count) <= 0)
        g_atomic_int_set (&db_data->ref_count, 1);
    else
        g_atomic_int_inc (&db_data->ref_count);

    return db_data;
}


void
database_data_unref (DatabaseData *db_data)
{
    if (db_data == NULL)
        return;

    if (g_atomic_int_get (&db_data->ref_count) > 0 &&
        !g_atomic_int_dec_and_test (&db_data->ref_count))
        return;

    database_data_purge_secrets (db_data);
    g_free (db_data->db_path);
    g_free (db_data);
}


void
database_data_free (DatabaseData *db_data)
{
    database_data_unref (db_data);
}


guint
db_get_quarantined_count (DatabaseData *db_data)
{
    if (db_data == NULL || db_data->quarantined_tokens == NULL)
        return 0;
    return (guint) json_array_size (db_data->quarantined_tokens);
}


/* Load-time policy: never let one bad token brick the whole database. Move every
 * token that fails validation into db_data->quarantined_tokens (preserved and
 * re-merged on save, surfaced in the UI for repair) so the remaining valid
 * tokens open normally. Still fails hard when the root is not a JSON array (the
 * file is structurally corrupt rather than merely holding a bad entry). Rebuilds
 * quarantined_tokens from scratch each call so the migration re-decrypt pass
 * does not double-count. */
static gboolean
partition_valid_tokens (DatabaseData  *db_data,
                        GError       **err)
{
    if (!json_is_array (db_data->in_memory_json_data)) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Database JSON root must be an array.");
        return FALSE;
    }
    if (db_data->quarantined_tokens != NULL)
        json_decref (db_data->quarantined_tokens);
    db_data->quarantined_tokens = json_array ();
    guint set_aside = otp_extract_invalid_tokens (db_data->in_memory_json_data,
                                                  db_data->quarantined_tokens);
    if (set_aside > 0)
        g_info ("Set aside %u token(s) that failed validation; opened the "
                "database with the remaining valid tokens.", set_aside);
    return TRUE;
}


void
load_db (DatabaseData    *db_data,
         GError         **err)
{
    if (!g_file_test (db_data->db_path, G_FILE_TEST_EXISTS)) {
        g_set_error (err, missing_file_gquark (), MISSING_FILE_ERRCODE, "Missing database file");
        db_data->in_memory_json_data = NULL;
        return;
    }

    db_data->current_db_version = get_db_version (db_data->db_path, err);
    if (err != NULL && *err != NULL)
        return;
    if (db_data->current_db_version > DB_VERSION) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Unsupported future database version %d.", db_data->current_db_version);
        return;
    }

    /* Bail-on-error pattern: decrypt_db / update_db return NULL or set *err on
     * failure. Previously we used g_return_if_fail (err == NULL || *err == NULL)
     * here, but that logs a CRITICAL every time the condition is false - i.e.
     * on every legitimate failure (wrong password, truncated file, etc.) - and
     * that noise made real bugs harder to spot in the journal. A plain early
     * return preserves the original control flow without the misuse. */
    gsize in_memory_json_len = 0;
    gchar *in_memory_json = decrypt_db (db_data, &in_memory_json_len, err);
    if (in_memory_json == NULL)
        return;

    json_error_t jerr;
    if (in_memory_json_len > 0 && in_memory_json[in_memory_json_len - 1] == '\0')
        in_memory_json_len--;
    db_data->in_memory_json_data = json_loadb (in_memory_json, in_memory_json_len, 0, &jerr);
    gcry_free (in_memory_json);
    if (db_data->in_memory_json_data == NULL) {
        g_set_error (err, memlock_error_gquark(), MEMLOCK_ERRCODE,
                     "Error while loading json data: %s", jerr.text);
        return;
    }
    /* Give any anonymous token (neither label nor issuer) a placeholder so a
     * single such entry does not make the whole database refuse to open
     * (issue #462). The names live in memory and persist on the next save. */
    guint repaired = otp_repair_database_root (db_data->in_memory_json_data);
    if (repaired > 0)
        g_info ("Assigned placeholder labels to %u anonymous token(s) on load.", repaired);
    if (!partition_valid_tokens (db_data, err))
        return;

    if (db_data->current_db_version < DB_VERSION || db_data->needs_legacy_kdf_migration) {
        update_db (db_data, err);
        if (err != NULL && *err != NULL)
            return;

        if (db_data->in_memory_json_data != NULL) {
            json_decref (db_data->in_memory_json_data);
        }
        g_slist_free_full (db_data->objects_hash, g_free);
        db_data->objects_hash = NULL;

        in_memory_json = decrypt_db (db_data, &in_memory_json_len, err);
        if (in_memory_json == NULL)
            return;

        if (in_memory_json_len > 0 && in_memory_json[in_memory_json_len - 1] == '\0')
            in_memory_json_len--;
        db_data->in_memory_json_data = json_loadb (in_memory_json, in_memory_json_len, 0, &jerr);
        gcry_free (in_memory_json);
        if (db_data->in_memory_json_data == NULL) {
            g_set_error (err, memlock_error_gquark(), MEMLOCK_ERRCODE,
                         "Error while loading json data: %s", jerr.text);
            return;
        }
        otp_repair_database_root (db_data->in_memory_json_data);
        if (!partition_valid_tokens (db_data, err))
            return;
    }

    rebuild_objects_hash (db_data);
    refresh_committed_snapshot (db_data);

    /* decrypt_db records the digest of the exact authenticated bytes. Do not
     * reopen by path here: that could bind stale-write protection to a
     * different file than the one whose GCM tag was verified. */
}


void
update_db (DatabaseData  *db_data,
           GError       **err)
{
    g_return_if_fail (err == NULL || *err == NULL);

    gboolean first_run = (db_data->in_memory_json_data == NULL);
    json_t *candidate = first_run ? json_array () : json_deep_copy (db_data->in_memory_json_data);
    if (candidate == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Failed to allocate a database update candidate.");
        restore_live_from_committed (db_data);
        return;
    }

    if (first_run) {
        db_data->argon2id_iter = ARGON2ID_DEFAULT_ITER;
        db_data->argon2id_memcost = ARGON2ID_DEFAULT_MC;
        db_data->argon2id_parallelism = ARGON2ID_DEFAULT_PARAL;
    }

    if (db_data->data_to_add != NULL) {
        g_slist_foreach (db_data->data_to_add, add_to_json, candidate);
    }

    if (!otp_validate_database_root (candidate, err)) {
        json_decref (candidate);
        restore_live_from_committed (db_data);
        return;
    }

    DbLock lock = { .fd = -1, .path = NULL };
    if (!lock_db (db_data->db_path, &lock, err)) {
        json_decref (candidate);
        restore_live_from_committed (db_data);
        return;
    }

    gboolean exists = g_file_test (db_data->db_path, G_FILE_TEST_EXISTS);
    if (exists && db_data->has_loaded_file_digest &&
        !loaded_file_digest_matches (db_data, err)) {
        unlock_db (&lock);
        json_decref (candidate);
        restore_live_from_committed (db_data);
        return;
    }

    if (exists && !backup_db (db_data->db_path, err)) {
        unlock_db (&lock);
        json_decref (candidate);
        restore_live_from_committed (db_data);
        return;
    }

    gboolean committed = encrypt_db (db_data, candidate, err);
    unlock_db (&lock);

    if (!committed) {
        json_decref (candidate);
        restore_live_from_committed (db_data);
        return;
    }

    if (db_data->in_memory_json_data != NULL)
        json_decref (db_data->in_memory_json_data);
    db_data->in_memory_json_data = candidate;
    db_data->current_db_version = DB_VERSION;
    db_data->needs_legacy_kdf_migration = FALSE;
    refresh_committed_snapshot (db_data);

    g_slist_free_full (db_data->data_to_add, (GDestroyNotify) json_decref);
    db_data->data_to_add = NULL;
    rebuild_objects_hash (db_data);

    compute_file_digest (db_data->db_path, db_data->loaded_file_digest, NULL);
    db_data->has_loaded_file_digest = TRUE;

    backup_db (db_data->db_path, NULL);
}


gboolean
db_transaction (DatabaseData   *db_data,
                DbMutationFunc  mutation,
                gpointer        user_data,
                GError        **err)
{
    g_return_val_if_fail (db_data != NULL, FALSE);
    g_return_val_if_fail (mutation != NULL, FALSE);
    g_return_val_if_fail (err == NULL || *err == NULL, FALSE);

    json_t *candidate = (db_data->in_memory_json_data != NULL)
        ? json_deep_copy (db_data->in_memory_json_data)
        : json_array ();
    if (candidate == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Failed to allocate a database transaction candidate.");
        return FALSE;
    }

    if (!mutation (candidate, user_data, err) ||
        !otp_validate_database_root (candidate, err)) {
        json_decref (candidate);
        return FALSE;
    }

    DbLock lock = { .fd = -1, .path = NULL };
    if (!lock_db (db_data->db_path, &lock, err)) {
        json_decref (candidate);
        return FALSE;
    }

    gboolean exists = g_file_test (db_data->db_path, G_FILE_TEST_EXISTS);
    if (exists && db_data->has_loaded_file_digest &&
        !loaded_file_digest_matches (db_data, err)) {
        unlock_db (&lock);
        json_decref (candidate);
        return FALSE;
    }
    if (exists && !backup_db (db_data->db_path, err)) {
        unlock_db (&lock);
        json_decref (candidate);
        return FALSE;
    }

    gboolean committed = encrypt_db (db_data, candidate, err);
    unlock_db (&lock);
    if (!committed) {
        json_decref (candidate);
        return FALSE;
    }

    if (db_data->in_memory_json_data != NULL)
        json_decref (db_data->in_memory_json_data);
    db_data->in_memory_json_data = candidate;
    db_data->current_db_version = DB_VERSION;
    db_data->needs_legacy_kdf_migration = FALSE;
    rebuild_objects_hash (db_data);
    refresh_committed_snapshot (db_data);
    compute_file_digest (db_data->db_path, db_data->loaded_file_digest, NULL);
    db_data->has_loaded_file_digest = TRUE;
    backup_db (db_data->db_path, NULL);
    return TRUE;
}


gboolean
db_change_password (DatabaseData  *db_data,
                    const gchar   *new_password,
                    GError       **err)
{
    g_return_val_if_fail (db_data != NULL, FALSE);
    g_return_val_if_fail (new_password != NULL, FALSE);
    g_return_val_if_fail (err == NULL || *err == NULL, FALSE);

    gchar *old_key = db_data->key;
    gchar *new_key = gcry_calloc_secure (strlen (new_password) + 1, 1);
    if (new_key == NULL) {
        g_set_error (err, secmem_alloc_error_gquark (), SECMEM_ALLOC_ERRCODE,
                     "Could not allocate secure memory for the new database password.");
        return FALSE;
    }
    memcpy (new_key, new_password, strlen (new_password) + 1);

    db_data->key = new_key;
    db_invalidate_kdf_cache (db_data);

    update_db (db_data, err);
    if (err != NULL && *err != NULL) {
        gcry_free (new_key);
        db_data->key = old_key;
        db_invalidate_kdf_cache (db_data);
        return FALSE;
    }

    if (old_key != NULL)
        gcry_free (old_key);
    return TRUE;
}


gboolean
db_update_kdf_params (DatabaseData *db_data,
                      gint32        iter,
                      gint32        memcost,
                      gint32        parallelism,
                      GError      **err)
{
    g_return_val_if_fail (db_data != NULL, FALSE);
    g_return_val_if_fail (err == NULL || *err == NULL, FALSE);

    if (iter < ARGON2ID_MIN_ITER || iter > ARGON2ID_MAX_ITER ||
        memcost < ARGON2ID_MIN_MC || memcost > ARGON2ID_MAX_MC ||
        parallelism < ARGON2ID_MIN_PARAL || parallelism > ARGON2ID_MAX_PARAL)
    {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "KDF parameters are outside supported bounds.");
        return FALSE;
    }

    gint32 old_iter = db_data->argon2id_iter;
    gint32 old_memcost = db_data->argon2id_memcost;
    gint32 old_parallelism = db_data->argon2id_parallelism;

    db_data->argon2id_iter = iter;
    db_data->argon2id_memcost = memcost;
    db_data->argon2id_parallelism = parallelism;
    db_invalidate_kdf_cache (db_data);

    update_db (db_data, err);
    if (err != NULL && *err != NULL) {
        db_data->argon2id_iter = old_iter;
        db_data->argon2id_memcost = old_memcost;
        db_data->argon2id_parallelism = old_parallelism;
        db_invalidate_kdf_cache (db_data);
        return FALSE;
    }

    return TRUE;
}


typedef struct {
    GSList *otps;
    OtpImportReport *report;
} ImportOtpsContext;


static gboolean
candidate_contains_object (json_t *candidate,
                           json_t *obj)
{
    gsize index;
    json_t *existing;

    json_array_foreach (candidate, index, existing) {
        if (json_equal (existing, obj))
            return TRUE;
    }
    return FALSE;
}


static gboolean
import_otps_mutation (json_t   *candidate,
                      gpointer  user_data,
                      GError  **err)
{
    (void) err;
    ImportOtpsContext *ctx = user_data;
    OtpImportReport local = {0, 0, 0};
    OtpImportReport *report = ctx->report != NULL ? ctx->report : &local;
    *report = (OtpImportReport) {0, 0, 0};

    gsize import_index = 0;
    for (GSList *l = ctx->otps; l != NULL; l = l->next, import_index++) {
        otp_t *otp = l->data;
        GError *validation_err = NULL;
        /* Keep an anonymous imported token instead of dropping it (issue #462);
         * a no-op if a parse-time repair already named it. */
        otp_repair_anonymous_import_token (otp, import_index);
        if (!otp_validate_import_token (otp, &validation_err)) {
            if (validation_err != NULL) {
                g_printerr ("Skipping invalid imported token: %s\n", validation_err->message);
                g_clear_error (&validation_err);
            }
            report->skipped_invalid++;
            continue;
        }

        json_t *obj = build_json_obj (otp->type, otp->account_name, otp->issuer,
                                      otp->secret, otp->digits, otp->algo,
                                      otp->period, otp->counter, otp->group);
        if (obj == NULL) {
            report->skipped_invalid++;
            continue;
        }

        if (candidate_contains_object (candidate, obj)) {
            json_decref (obj);
            report->skipped_duplicates++;
            continue;
        }

        json_array_append_new (candidate, obj);
        report->added++;
    }

    return TRUE;
}


gboolean
db_import_otps (DatabaseData     *db_data,
                GSList           *otps,
                OtpImportReport  *report,
                GError         **err)
{
    ImportOtpsContext ctx = {
        .otps = otps,
        .report = report,
    };
    return db_transaction (db_data, import_otps_mutation, &ctx, err);
}


/* True when `obj` is byte-equal (per Jansson's json_equal) to any token already
 * present either on disk or staged for the next save. Used to break ties when
 * two distinct OTP tokens share a 32-bit jenkins hash - without this fallback
 * the second token would be silently skipped as a "duplicate". */
static gboolean
otp_object_already_present (DatabaseData *db_data,
                            json_t       *obj)
{
    if (db_data->in_memory_json_data != NULL) {
        gsize idx;
        json_t *existing;
        json_array_foreach (db_data->in_memory_json_data, idx, existing) {
            if (json_equal (existing, obj))
                return TRUE;
        }
    }
    for (GSList *l = db_data->data_to_add; l != NULL; l = l->next) {
        if (json_equal ((json_t *) l->data, obj))
            return TRUE;
    }
    return FALSE;
}


static void
rebuild_objects_hash (DatabaseData *db_data)
{
    g_slist_free_full (db_data->objects_hash, g_free);
    db_data->objects_hash = NULL;

    if (db_data->in_memory_json_data == NULL)
        return;

    gsize index;
    json_t *obj;
    json_array_foreach (db_data->in_memory_json_data, index, obj) {
        guint32 hash = json_object_get_hash (obj);
        db_data->objects_hash = g_slist_prepend (db_data->objects_hash,
                                                 g_memdup2 (&hash, sizeof (guint32)));
    }
    db_data->objects_hash = g_slist_reverse (db_data->objects_hash);
}


static void
refresh_committed_snapshot (DatabaseData *db_data)
{
    if (db_data->committed_json_data != NULL) {
        json_decref (db_data->committed_json_data);
        db_data->committed_json_data = NULL;
    }
    if (db_data->in_memory_json_data != NULL)
        db_data->committed_json_data = json_deep_copy (db_data->in_memory_json_data);
}


static void
restore_live_from_committed (DatabaseData *db_data)
{
    if (db_data->committed_json_data == NULL)
        return;
    if (db_data->in_memory_json_data != NULL)
        json_decref (db_data->in_memory_json_data);
    db_data->in_memory_json_data = json_deep_copy (db_data->committed_json_data);
    rebuild_objects_hash (db_data);
}


void
add_otps_to_db_ex (GSList       *otps,
                   DatabaseData *db_data,
                   guint        *added_out,
                   guint        *skipped_out)
{
    guint added = 0, skipped = 0;
    GSList *new_hashes = NULL;
    GSList *new_data = NULL;
    gsize import_index = 0;
    for (GSList *l = otps; l != NULL; l = l->next, import_index++) {
        otp_t *otp = l->data;
        GError *validation_err = NULL;
        /* Keep an anonymous imported token instead of dropping it (issue #462);
         * a no-op if a parse-time repair already named it. */
        otp_repair_anonymous_import_token (otp, import_index);
        if (!otp_validate_import_token (otp, &validation_err)) {
            if (validation_err != NULL) {
                g_printerr ("Skipping invalid imported token: %s\n", validation_err->message);
                g_clear_error (&validation_err);
            }
            skipped++;
            continue;
        }
        json_t *obj = build_json_obj (otp->type, otp->account_name, otp->issuer, otp->secret, otp->digits, otp->algo, otp->period, otp->counter, otp->group);
        guint32 hash = json_object_get_hash (obj);
        gboolean is_duplicate = FALSE;
        if (g_slist_find_custom (db_data->objects_hash, GUINT_TO_POINTER((guint)hash), check_duplicate) != NULL) {
            // Hash hit: confirm with a real content comparison before skipping.
            // jenkins is 32-bit and the hashed string is bounded; collisions
            // between distinct tokens are realistic and silent skipping causes
            // user-visible data loss on import.
            is_duplicate = otp_object_already_present (db_data, obj);
        }
        if (!is_duplicate &&
            g_slist_find_custom (new_hashes, GUINT_TO_POINTER((guint)hash), check_duplicate) != NULL) {
            for (GSList *staged = new_data; staged != NULL; staged = staged->next) {
                if (json_equal ((json_t *) staged->data, obj)) {
                    is_duplicate = TRUE;
                    break;
                }
            }
        }
        if (!is_duplicate) {
            new_hashes = g_slist_prepend (new_hashes, g_memdup2 (&hash, sizeof (guint32)));
            new_data = g_slist_prepend (new_data, obj);
            added++;
        } else {
            json_decref (obj);
            skipped++;
        }
    }
    db_data->objects_hash = g_slist_concat (db_data->objects_hash, g_slist_reverse (new_hashes));
    db_data->data_to_add = g_slist_concat (db_data->data_to_add, g_slist_reverse (new_data));
    if (added_out != NULL) *added_out = added;
    if (skipped_out != NULL) *skipped_out = skipped;
}


void
add_otps_to_db (GSList       *otps,
                DatabaseData *db_data)
{
    add_otps_to_db_ex (otps, db_data, NULL, NULL);
}


gint
check_duplicate (gconstpointer data,
                 gconstpointer user_data)
{
    guint32 list_elem = *(guint32 *)data;
    if (list_elem == (guint32)GPOINTER_TO_UINT(user_data)) {
        return 0;
    }
    return -1;
}


static gint32
get_db_version (const gchar  *db_path,
                GError      **err)
{
    GFile *in_file = g_file_new_for_path (db_path);
    GFileInputStream *in_stream = g_file_read (in_file, NULL, err);
    if (!in_stream) {
        cleanup_db_gfile (in_file, NULL, NULL);
        return -1;
    }

    guint8 header[DB_HEADER_NAME_LEN + 4] = {0};
    gsize bytes_read = 0;
    if (!g_input_stream_read_all (G_INPUT_STREAM(in_stream), header, sizeof (header),
                                  &bytes_read, NULL, err)) {
        cleanup_db_gfile (in_file, in_stream, NULL);
        return -1;
    }

    if (bytes_read < DB_HEADER_NAME_LEN) {
        cleanup_db_gfile (in_file, in_stream, NULL);
        return 1;
    }
    if (memcmp (DB_HEADER_NAME, header, DB_HEADER_NAME_LEN) != 0) {
        cleanup_db_gfile (in_file, in_stream, NULL);
        return 1;
    }
    if (bytes_read < sizeof (header)) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Database header is truncated.");
        cleanup_db_gfile (in_file, in_stream, NULL);
        return -1;
    }

    // v3 writes the version explicitly with write_be32, so bytes [9..12] are
    // 00 00 00 03. v2 wrote a zero-initialized DbHeaderData_v2 struct whose
    // 3 bytes of padding before `gint32 db_version` are zero, followed by the
    // LE int32 value 2 - i.e. bytes [9..12] are 00 00 00 02. Both layouts
    // round-trip cleanly through read_be32. A previous attempt fell back to a
    // native-endian memcpy when read_be32 != 3, which turned a v2 file's
    // 00 00 00 02 into 0x02000000 (33554432) and tripped the "future version"
    // guard, locking users out of legitimate v2 databases.
    guint32 version = read_be32 (header + DB_HEADER_NAME_LEN);
    if (version < 2)
        version = 2;
    cleanup_db_gfile (in_file, in_stream, NULL);
    return (gint32) version;
}


static guchar *
get_db_derived_key (DatabaseData  *db_data,
                    gint32         db_version,
                    const guint8  *salt,
                    gboolean       use_legacy_length,
                    GError       **err)
{
    // gcry_kdf_* expects the password length in BYTES, but historically this
    // code passed g_utf8_strlen (CHARACTER count), truncating non-ASCII passwords
    // mid-byte and weakening the KDF. strlen is correct; use_legacy_length=TRUE
    // is only set on the retry path used to read databases written by older
    // OTPClient versions (see try_decrypt_v2 / decrypt_db).
    gsize pwd_len = use_legacy_length
                    ? (gsize) g_utf8_strlen (db_data->key, -1)
                    : strlen (db_data->key);

    guchar *derived_key = NULL;
    if (db_version < 2) {
        // v1 (PBKDF2) databases were always written with the legacy length, so
        // their decrypt path keeps the legacy behavior. After successful load
        // the caller migrates the DB to v2 with the corrected length.
        pwd_len = (gsize) g_utf8_strlen (db_data->key, -1);

        gsize key_len = gcry_cipher_get_algo_keylen (GCRY_CIPHER_AES256);

        derived_key = gcry_malloc_secure (key_len);
        if (derived_key == NULL) {
            g_set_error (err, secmem_alloc_error_gquark (), SECMEM_ALLOC_ERRCODE, "Error while allocating secure memory.");
            return NULL;
        }

        if (gcry_kdf_derive (db_data->key, pwd_len,
                             GCRY_KDF_PBKDF2, GCRY_MD_SHA512,
                             salt, KDF_SALT_SIZE,
                             KDF_ITERATIONS, key_len, derived_key) != GPG_ERR_NO_ERROR) {
            gcry_free (derived_key);
            g_set_error (err, key_deriv_gquark (), KEY_DERIVATION_ERRCODE, "Error while deriving the key.");
            return NULL;
        }
    } else {
        // Cache hit: same salt AND same password as the last successful
        // derive => same derived key (KDF params haven't changed, since the
        // cache is invalidated on password change). The password-hash check
        // is what stops a cached key from a prior successful unlock being
        // returned to a later attempt with a different password - without it
        // the salt-only lookup would accept any password against a cache
        // entry from the same file.
        if (!use_legacy_length
            && db_data->has_cached_key
            && db_data->cached_derived_key != NULL
            && memcmp (db_data->cached_salt, salt, KDF_SALT_SIZE) == 0) {
            guint8 pwd_hash[32];
            gcry_md_hash_buffer (GCRY_MD_SHA256, pwd_hash, db_data->key, pwd_len);
            gboolean pwd_matches = (memcmp (db_data->cached_pwd_hash, pwd_hash, sizeof (pwd_hash)) == 0);
            explicit_bzero (pwd_hash, sizeof (pwd_hash));
            if (pwd_matches) {
                derived_key = gcry_malloc_secure (ARGON2ID_KEYLEN);
                if (derived_key == NULL) {
                    g_set_error (err, secmem_alloc_error_gquark (), SECMEM_ALLOC_ERRCODE, "Error while allocating secure memory.");
                    return NULL;
                }
                memcpy (derived_key, db_data->cached_derived_key, ARGON2ID_KEYLEN);
                return derived_key;
            }
        }

        derived_key = gcry_malloc_secure (ARGON2ID_KEYLEN);
        if (derived_key == NULL) {
            g_set_error (err, secmem_alloc_error_gquark (),
                         SECMEM_ALLOC_ERRCODE,
                         "Error while allocating secure memory.");
            return NULL;
        }
        const unsigned long params[4] = {ARGON2ID_TAGLEN, db_data->argon2id_iter, db_data->argon2id_memcost, db_data->argon2id_parallelism};
        gcry_kdf_hd_t hd;
        if (gcry_kdf_open (&hd, GCRY_KDF_ARGON2, GCRY_KDF_ARGON2ID,
                           params, 4,
                           db_data->key, pwd_len,
                           salt, KDF_SALT_SIZE,
                           NULL, 0, NULL, 0) != GPG_ERR_NO_ERROR) {
            gcry_free (derived_key);
            g_set_error (err, key_deriv_gquark (), KEY_DERIVATION_ERRCODE, "Error while deriving the key (kdf_open).");
            return NULL;
        }
        if (gcry_kdf_compute (hd, NULL) != GPG_ERR_NO_ERROR) {
            gcry_free (derived_key);
            gcry_kdf_close (hd);
            g_set_error (err, key_deriv_gquark (), KEY_DERIVATION_ERRCODE, "Error while deriving the key (kdf_compute).");
            return NULL;
        }
        if (gcry_kdf_final (hd, ARGON2ID_KEYLEN, derived_key) != GPG_ERR_NO_ERROR) {
            gcry_free (derived_key);
            gcry_kdf_close (hd);
            g_set_error (err, key_deriv_gquark (), KEY_DERIVATION_ERRCODE, "Error while deriving the key (kdf_final).");
            return NULL;
        }
        gcry_kdf_close (hd);
    }

    return derived_key;
}


static gchar *
try_decrypt_v2 (DatabaseData    *db_data,
                DbHeaderData_v2 *header_data,
                guchar          *enc_buf,
                gsize            enc_buf_size,
                const guchar    *tag,
                gboolean         use_legacy_length,
                GError         **err)
{
    guchar *derived_key = get_db_derived_key (db_data, db_data->current_db_version, header_data->salt, use_legacy_length, err);
    if (derived_key == NULL) {
        return NULL;
    }

    gcry_cipher_hd_t hd = open_cipher_and_set_data (derived_key, header_data->iv, IV_SIZE);
    if (!hd) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Error while opening and setting the cipher data.");
        explicit_bzero (derived_key, ARGON2ID_KEYLEN);
        gcry_free (derived_key);
        return NULL;
    }

    if (gcry_cipher_authenticate (hd, header_data, sizeof(DbHeaderData_v2)) != GPG_ERR_NO_ERROR) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Error while processing the authenticated data.");
        gcry_cipher_close (hd);
        explicit_bzero (derived_key, ARGON2ID_KEYLEN);
        gcry_free (derived_key);
        return NULL;
    }

    gchar *dec_buf = gcry_calloc_secure (enc_buf_size + 1, 1);
    if (!dec_buf) {
        g_set_error (err, secmem_alloc_error_gquark (), SECMEM_ALLOC_ERRCODE, "Error while allocating secure memory.");
        gcry_cipher_close (hd);
        explicit_bzero (derived_key, ARGON2ID_KEYLEN);
        gcry_free (derived_key);
        return NULL;
    }

    if (gcry_cipher_decrypt (hd, dec_buf, enc_buf_size, enc_buf, enc_buf_size) != GPG_ERR_NO_ERROR) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Error while decrypting the data.");
        gcry_cipher_close (hd);
        explicit_bzero (derived_key, ARGON2ID_KEYLEN);
        gcry_free (derived_key);
        gcry_free (dec_buf);
        return NULL;
    }

    // Catch every tag-verification failure, not just CHECKSUM. A non-CHECKSUM
    // error (INV_LENGTH, NO_KEY, ...) would otherwise let unverified plaintext
    // be returned as if the tag had matched.
    gpg_error_t tag_err = gcry_cipher_checktag (hd, tag, TAG_SIZE);
    if (tag_err != GPG_ERR_NO_ERROR) {
        if (gcry_err_code (tag_err) == GPG_ERR_CHECKSUM) {
            g_set_error (err, bad_tag_gquark (), BAD_TAG_ERRCODE, "The tag doesn't match. Either the password is wrong or the file is corrupted.");
        } else {
            g_set_error (err, bad_tag_gquark (), BAD_TAG_ERRCODE,
                         "Tag verification failed: %s/%s",
                         gcry_strsource (tag_err), gcry_strerror (tag_err));
        }
        gcry_cipher_close (hd);
        explicit_bzero (derived_key, ARGON2ID_KEYLEN);
        gcry_free (derived_key);
        gcry_free (dec_buf);
        return NULL;
    }

    gcry_cipher_close (hd);

    // Cache the derived key ONLY after the tag verifies. Caching pre-tag
    // (the old behaviour) poisoned the cache with the wrong-password-derived
    // key on a failed attempt; the salt-keyed lookup then returned that wrong
    // key to subsequent correct-password attempts, producing an unlock loop
    // that only ended at process exit (#448). The legacy-length retry is
    // one-shot for pre-fix files and must not populate the cache.
    if (!use_legacy_length) {
        if (db_data->cached_derived_key == NULL)
            db_data->cached_derived_key = gcry_malloc_secure (ARGON2ID_KEYLEN);
        if (db_data->cached_derived_key != NULL) {
            memcpy (db_data->cached_derived_key, derived_key, ARGON2ID_KEYLEN);
            memcpy (db_data->cached_salt, header_data->salt, KDF_SALT_SIZE);
            gcry_md_hash_buffer (GCRY_MD_SHA256, db_data->cached_pwd_hash,
                                 db_data->key, strlen (db_data->key));
            db_data->has_cached_key = TRUE;
        }
    }

    explicit_bzero (derived_key, ARGON2ID_KEYLEN);
    gcry_free (derived_key);
    return dec_buf;
}


static gchar *
try_decrypt_v3 (DatabaseData  *db_data,
                const guint8  *header_data,
                gsize          header_data_size,
                const guint8  *salt,
                const guint8  *iv,
                guchar        *enc_buf,
                gsize          enc_buf_size,
                const guchar  *tag,
                gboolean       use_legacy_length,
                GError       **err)
{
    guchar *derived_key = get_db_derived_key (db_data, DB_VERSION, salt,
                                              use_legacy_length, err);
    if (derived_key == NULL)
        return NULL;

    gcry_cipher_hd_t hd = open_cipher_and_set_data (derived_key, (guchar *) iv, IV_SIZE);
    if (!hd) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Error while opening and setting the cipher data.");
        explicit_bzero (derived_key, ARGON2ID_KEYLEN);
        gcry_free (derived_key);
        return NULL;
    }

    if (gcry_cipher_authenticate (hd, header_data, header_data_size) != GPG_ERR_NO_ERROR) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Error while processing the authenticated data.");
        gcry_cipher_close (hd);
        explicit_bzero (derived_key, ARGON2ID_KEYLEN);
        gcry_free (derived_key);
        return NULL;
    }

    gchar *dec_buf = gcry_calloc_secure (enc_buf_size + 1, 1);
    if (!dec_buf) {
        g_set_error (err, secmem_alloc_error_gquark (), SECMEM_ALLOC_ERRCODE,
                     "Error while allocating secure memory.");
        gcry_cipher_close (hd);
        explicit_bzero (derived_key, ARGON2ID_KEYLEN);
        gcry_free (derived_key);
        return NULL;
    }

    if (gcry_cipher_decrypt (hd, dec_buf, enc_buf_size, enc_buf, enc_buf_size) != GPG_ERR_NO_ERROR) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Error while decrypting the data.");
        gcry_cipher_close (hd);
        explicit_bzero (derived_key, ARGON2ID_KEYLEN);
        gcry_free (derived_key);
        gcry_free (dec_buf);
        return NULL;
    }

    gpg_error_t tag_err = gcry_cipher_checktag (hd, tag, TAG_SIZE);
    if (tag_err != GPG_ERR_NO_ERROR) {
        g_set_error (err, bad_tag_gquark (), BAD_TAG_ERRCODE,
                     "Tag verification failed: %s/%s",
                     gcry_strsource (tag_err), gcry_strerror (tag_err));
        gcry_cipher_close (hd);
        explicit_bzero (derived_key, ARGON2ID_KEYLEN);
        gcry_free (derived_key);
        gcry_free (dec_buf);
        return NULL;
    }

    gcry_cipher_close (hd);

    if (!use_legacy_length) {
        if (db_data->cached_derived_key == NULL)
            db_data->cached_derived_key = gcry_malloc_secure (ARGON2ID_KEYLEN);
        if (db_data->cached_derived_key != NULL) {
            memcpy (db_data->cached_derived_key, derived_key, ARGON2ID_KEYLEN);
            memcpy (db_data->cached_salt, salt, KDF_SALT_SIZE);
            gcry_md_hash_buffer (GCRY_MD_SHA256, db_data->cached_pwd_hash,
                                 db_data->key, strlen (db_data->key));
            db_data->has_cached_key = TRUE;
        }
    }

    explicit_bzero (derived_key, ARGON2ID_KEYLEN);
    gcry_free (derived_key);
    return dec_buf;
}


static gchar *
decrypt_db (DatabaseData *db_data,
            gsize        *dec_len,
            GError      **err)
{
    g_return_val_if_fail (err == NULL || *err == NULL, NULL);

    int fd = path_open_safe_regular_file (db_data->db_path, err);
    if (fd < 0)
        return NULL;

    struct stat st;
    if (fstat (fd, &st) != 0) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Cannot stat database file: %s", g_strerror (errno));
        close (fd);
        return NULL;
    }

    goffset input_file_size = st.st_size;
    if (input_file_size <= 0 || input_file_size > G_MAXSIZE) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Database file size is invalid.");
        close (fd);
        return NULL;
    }
    if (db_data->max_file_size_from_memlock > 0 &&
        input_file_size > (goffset) (db_data->max_file_size_from_memlock * SECMEM_SIZE_THRESHOLD_RATIO)) {
        g_set_error (err, file_too_big_gquark (), FILE_TOO_BIG_ERRCODE, FILE_SIZE_SECMEM_MSG);
        close (fd);
        return NULL;
    }

    gsize file_size = (gsize) input_file_size;
    guint8 *file_buf = g_malloc0 (file_size);
    gsize total_read = 0;
    while (total_read < file_size) {
        ssize_t n = read (fd, file_buf + total_read, file_size - total_read);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                         "Failed to read database file: %s", g_strerror (errno));
            close (fd);
            g_free (file_buf);
            return NULL;
        }
        if (n == 0) {
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                         "Short read while reading database file.");
            close (fd);
            g_free (file_buf);
            return NULL;
        }
        total_read += (gsize) n;
    }
    close (fd);

    gsize header_data_size = 0;
    if (db_data->current_db_version == 1)
        header_data_size = sizeof (DbHeaderData_v1);
    else if (db_data->current_db_version == 2)
        header_data_size = sizeof (DbHeaderData_v2);
    else if (db_data->current_db_version == 3)
        header_data_size = DB_V3_HEADER_SIZE;
    else {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Unsupported database version %d.", db_data->current_db_version);
        g_free (file_buf);
        return NULL;
    }

    if (file_size < header_data_size + TAG_SIZE) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Database file is too small (got %" G_GSIZE_FORMAT " bytes, need at least %" G_GSIZE_FORMAT ").",
                     file_size, header_data_size + TAG_SIZE);
        g_free (file_buf);
        return NULL;
    }

    const guint8 *tag = file_buf + file_size - TAG_SIZE;
    guchar *enc_buf = file_buf + header_data_size;
    gsize enc_buf_size = file_size - header_data_size - TAG_SIZE;
    if (dec_len != NULL)
        *dec_len = enc_buf_size;

    gchar *dec_buf = NULL;
    if (db_data->current_db_version == 3) {
        const guint8 *header = file_buf;
        if (memcmp (header, DB_HEADER_NAME, DB_HEADER_NAME_LEN) != 0 ||
            read_be32 (header + DB_HEADER_NAME_LEN) != 3) {
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                         "Malformed database v3 header.");
            g_free (file_buf);
            return NULL;
        }

        const guint8 *iv = header + DB_HEADER_NAME_LEN + 4;
        const guint8 *salt = iv + IV_SIZE;
        db_data->argon2id_iter = (gint32) read_be32 (salt + KDF_SALT_SIZE);
        db_data->argon2id_memcost = (gint32) read_be32 (salt + KDF_SALT_SIZE + 4);
        db_data->argon2id_parallelism = (gint32) read_be32 (salt + KDF_SALT_SIZE + 8);

        if (db_data->argon2id_iter < ARGON2ID_MIN_ITER || db_data->argon2id_iter > ARGON2ID_MAX_ITER ||
            db_data->argon2id_memcost < ARGON2ID_MIN_MC || db_data->argon2id_memcost > ARGON2ID_MAX_MC ||
            db_data->argon2id_parallelism < ARGON2ID_MIN_PARAL || db_data->argon2id_parallelism > ARGON2ID_MAX_PARAL) {
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                         "Database header contains out-of-range Argon2id parameters "
                         "(iter=%d, memcost=%d KiB, parallelism=%d).",
                         db_data->argon2id_iter, db_data->argon2id_memcost,
                         db_data->argon2id_parallelism);
            g_free (file_buf);
            return NULL;
        }

        dec_buf = try_decrypt_v3 (db_data, header, header_data_size, salt, iv,
                                  enc_buf, enc_buf_size, tag, FALSE, err);
    } else if (db_data->current_db_version == 2) {
        DbHeaderData_v2 *header_data_v2 = g_new0 (DbHeaderData_v2, 1);
        memcpy (header_data_v2, file_buf, sizeof (DbHeaderData_v2));
        db_data->argon2id_iter = header_data_v2->argon2id_iter;
        db_data->argon2id_memcost = header_data_v2->argon2id_memcost;
        db_data->argon2id_parallelism = header_data_v2->argon2id_parallelism;

        if (db_data->argon2id_iter < ARGON2ID_MIN_ITER || db_data->argon2id_iter > ARGON2ID_MAX_ITER ||
            db_data->argon2id_memcost < ARGON2ID_MIN_MC || db_data->argon2id_memcost > ARGON2ID_MAX_MC ||
            db_data->argon2id_parallelism < ARGON2ID_MIN_PARAL || db_data->argon2id_parallelism > ARGON2ID_MAX_PARAL) {
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                         "Database header contains out-of-range Argon2id parameters "
                         "(iter=%d, memcost=%d KiB, parallelism=%d).",
                         db_data->argon2id_iter, db_data->argon2id_memcost,
                         db_data->argon2id_parallelism);
            g_free (header_data_v2);
            g_free (file_buf);
            return NULL;
        }

        dec_buf = try_decrypt_v2 (db_data, header_data_v2, enc_buf, enc_buf_size, tag, FALSE, err);
        if (dec_buf == NULL && err != NULL && *err != NULL && (*err)->domain == bad_tag_gquark ()) {
            gsize byte_len = strlen (db_data->key);
            gsize char_len = (gsize) g_utf8_strlen (db_data->key, -1);
            if (byte_len != char_len) {
                g_clear_error (err);
                dec_buf = try_decrypt_v2 (db_data, header_data_v2, enc_buf, enc_buf_size, tag, TRUE, err);
                if (dec_buf != NULL)
                    db_data->needs_legacy_kdf_migration = TRUE;
            }
        }
        g_free (header_data_v2);
    } else {
        DbHeaderData_v1 *header_data_v1 = g_new0 (DbHeaderData_v1, 1);
        memcpy (header_data_v1, file_buf, sizeof (DbHeaderData_v1));
        db_data->argon2id_iter = ARGON2ID_DEFAULT_ITER;
        db_data->argon2id_memcost = ARGON2ID_DEFAULT_MC;
        db_data->argon2id_parallelism = ARGON2ID_DEFAULT_PARAL;

        guchar *derived_key = get_db_derived_key (db_data, db_data->current_db_version,
                                                  header_data_v1->salt, FALSE, err);
        if (derived_key != NULL) {
            gcry_cipher_hd_t hd = open_cipher_and_set_data (derived_key, header_data_v1->iv, IV_SIZE);
            if (hd == NULL) {
                g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                             "Error while opening and setting the cipher data.");
            } else if (gcry_cipher_authenticate (hd, header_data_v1, header_data_size) != GPG_ERR_NO_ERROR) {
                g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                             "Error while processing the authenticated data.");
            } else {
                dec_buf = gcry_calloc_secure (enc_buf_size + 1, 1);
                if (dec_buf == NULL) {
                    g_set_error (err, secmem_alloc_error_gquark (), SECMEM_ALLOC_ERRCODE,
                                 "Error while allocating secure memory.");
                } else if (gcry_cipher_decrypt (hd, dec_buf, enc_buf_size, enc_buf, enc_buf_size) != GPG_ERR_NO_ERROR) {
                    g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                                 "Error while decrypting the data.");
                    gcry_free (dec_buf);
                    dec_buf = NULL;
                } else {
                    gpg_error_t tag_err = gcry_cipher_checktag (hd, tag, TAG_SIZE);
                    if (tag_err != GPG_ERR_NO_ERROR) {
                        g_set_error (err, bad_tag_gquark (), BAD_TAG_ERRCODE,
                                     "Tag verification failed: %s/%s",
                                     gcry_strsource (tag_err), gcry_strerror (tag_err));
                        gcry_free (dec_buf);
                        dec_buf = NULL;
                    }
                }
            }
            if (hd != NULL)
                gcry_cipher_close (hd);
            explicit_bzero (derived_key, ARGON2ID_KEYLEN);
            gcry_free (derived_key);
        }
        g_free (header_data_v1);
    }

    if (dec_buf != NULL) {
        gcry_md_hash_buffer (GCRY_MD_SHA256, db_data->loaded_file_digest,
                             file_buf, file_size);
        db_data->has_loaded_file_digest = TRUE;
    }
    g_free (file_buf);
    return dec_buf;
}


static gboolean
encrypt_db (DatabaseData *db_data,
            json_t       *json_data,
            GError      **err)
{
    g_return_val_if_fail (err == NULL || *err == NULL, FALSE);

#ifdef OTPCLIENT_TESTING
    if (test_fail_encrypt) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Injected encrypt failure.");
        return FALSE;
    }
#endif

    guint8 header_data[DB_V3_HEADER_SIZE] = {0};
    memcpy (header_data, DB_HEADER_NAME, DB_HEADER_NAME_LEN);
    write_be32 (header_data + DB_HEADER_NAME_LEN, DB_VERSION);
    guint8 *iv = header_data + DB_HEADER_NAME_LEN + 4;
    guint8 *salt = iv + IV_SIZE;
    gcry_create_nonce (iv, IV_SIZE);
    // Reuse the previously-derived salt when we have a cached key so the KDF
    // step is a memcpy instead of a 150 ms Argon2id derivation. The per-save
    // random IV above guarantees AES-GCM nonce uniqueness independently of
    // salt reuse. db_invalidate_kdf_cache() is called on password change to
    // force a fresh salt + key on the next save.
    if (db_data->has_cached_key)
        memcpy (salt, db_data->cached_salt, KDF_SALT_SIZE);
    else
        gcry_create_nonce (salt, KDF_SALT_SIZE);
    write_be32 (salt + KDF_SALT_SIZE, (guint32) db_data->argon2id_iter);
    write_be32 (salt + KDF_SALT_SIZE + 4, (guint32) db_data->argon2id_memcost);
    write_be32 (salt + KDF_SALT_SIZE + 8, (guint32) db_data->argon2id_parallelism);

    // encrypt_db unconditionally uses the corrected (strlen) password byte length.
    // The legacy g_utf8_strlen length is only used on the decrypt retry path
    // when reading older databases (see decrypt_db / try_decrypt_v2).
    guchar *derived_key = get_db_derived_key (db_data, DB_VERSION, salt, FALSE, err);
    if (derived_key == NULL) {
        return FALSE;
    }

    // Preserve tokens set aside on load because they failed validation (issue
    // #464): they are kept out of the live in-memory set, so merge them back in
    // for serialization only, leaving the caller's json_data untouched. merged is
    // a shallow copy referencing the same token objects, so freeing it never
    // touches the underlying secrets.
    json_t *to_dump = json_data;
    json_t *merged = NULL;
    if (db_data->quarantined_tokens != NULL &&
        json_array_size (db_data->quarantined_tokens) > 0) {
        merged = json_copy (json_data);
        if (merged == NULL) {
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                         "Failed to merge preserved tokens for serialization.");
            explicit_bzero (derived_key, ARGON2ID_KEYLEN);
            gcry_free (derived_key);
            return FALSE;
        }
        gsize q_idx;
        json_t *q_obj;
        json_array_foreach (db_data->quarantined_tokens, q_idx, q_obj)
            json_array_append (merged, q_obj);
        to_dump = merged;
    }

    gsize input_data_len = json_dumpb (to_dump, NULL, 0, JSON_COMPACT);
    if (input_data_len == 0) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed to serialize the in-memory database.");
        if (merged != NULL) json_decref (merged);
        explicit_bzero (derived_key, ARGON2ID_KEYLEN);
        gcry_free (derived_key);
        return FALSE;
    }
    gchar *in_memory_dumped_data = gcry_calloc_secure (input_data_len, 1);
    if (in_memory_dumped_data == NULL) {
        g_set_error (err, secmem_alloc_error_gquark (), SECMEM_ALLOC_ERRCODE,
                     "Failed to allocate secure memory for serialized database.");
        if (merged != NULL) json_decref (merged);
        explicit_bzero (derived_key, ARGON2ID_KEYLEN);
        gcry_free (derived_key);
        return FALSE;
    }
    if (json_dumpb (to_dump, in_memory_dumped_data, input_data_len, JSON_COMPACT) == (size_t) -1) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed to serialize the in-memory database.");
        if (merged != NULL) json_decref (merged);
        gcry_free (in_memory_dumped_data);
        explicit_bzero (derived_key, ARGON2ID_KEYLEN);
        gcry_free (derived_key);
        return FALSE;
    }
    if (merged != NULL) json_decref (merged);
    guchar *enc_buffer = g_malloc0 (input_data_len);

    // C3 fix: in_memory_dumped_data holds the plaintext database (every secret).
    // Every error path below MUST free it; previously only the success path did,
    // leaving plaintext in secure memory until process exit on encrypt failures.
    gcry_cipher_hd_t hd = open_cipher_and_set_data (derived_key, iv, IV_SIZE);
    if (hd == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed to open the cipher and set the data.");
        gcry_free (in_memory_dumped_data);
        free_db_resources (NULL, derived_key, enc_buffer, NULL, NULL, NULL);
        return FALSE;
    }

    if (gcry_cipher_authenticate (hd, header_data, sizeof (header_data)) != GPG_ERR_NO_ERROR) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Error while processing the authenticated data");
        gcry_free (in_memory_dumped_data);
        free_db_resources (hd, derived_key, enc_buffer, NULL, NULL, NULL);
        return FALSE;
    }

    if (gcry_cipher_encrypt (hd, enc_buffer, input_data_len, in_memory_dumped_data, input_data_len) != GPG_ERR_NO_ERROR) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Error while encrypting the data.");
        gcry_free (in_memory_dumped_data);
        free_db_resources (hd, derived_key, enc_buffer, NULL, NULL, NULL);
        return FALSE;
    }
    gcry_free (in_memory_dumped_data);

    guchar tag[TAG_SIZE];
    if (gcry_cipher_gettag (hd, tag, TAG_SIZE) != GPG_ERR_NO_ERROR) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Error while getting the tag.");
        free_db_resources (hd, derived_key, enc_buffer, NULL, NULL, NULL);
        return FALSE;
    }

    gboolean wrote = atomic_write_database (db_data->db_path, header_data, sizeof (header_data),
                                            enc_buffer, input_data_len, tag, err);
    explicit_bzero (tag, TAG_SIZE);
    if (!wrote) {
        free_db_resources (hd, derived_key, enc_buffer, NULL, NULL, NULL);
        return FALSE;
    }

    // Mirror try_decrypt_v2's cache populate so subsequent operations under
    // the same (password, salt) skip the 150 ms Argon2id derivation. Without
    // this, the first save after a fresh unlock paid the derive cost but
    // discarded the result, forcing the next save to redo the work.
    if (db_data->cached_derived_key == NULL)
        db_data->cached_derived_key = gcry_malloc_secure (ARGON2ID_KEYLEN);
    if (db_data->cached_derived_key != NULL) {
        memcpy (db_data->cached_derived_key, derived_key, ARGON2ID_KEYLEN);
        memcpy (db_data->cached_salt, salt, KDF_SALT_SIZE);
        gcry_md_hash_buffer (GCRY_MD_SHA256, db_data->cached_pwd_hash,
                             db_data->key, strlen (db_data->key));
        db_data->has_cached_key = TRUE;
    }

    free_db_resources (hd, derived_key, enc_buffer, NULL, NULL, NULL);

    db_data->current_db_version = DB_VERSION;
    db_data->needs_legacy_kdf_migration = FALSE;
    return TRUE;
}


static void
add_to_json (gpointer list_elem,
             gpointer json_array)
{
    // append_new (not append) for the freshly-created deep copy: append would
    // bump the deep-copy's refcount to 2 with no local handle to decref, so
    // the copy would persist indefinitely after the parent array is freed.
    json_array_append_new (json_array, json_deep_copy (list_elem));
}


static gboolean
perform_backup_restore (const gchar *path,
                        gboolean     is_backup,
                        GError     **out_err)
{
    GError *err = NULL;
    gchar *src_path = is_backup ? g_strdup (path) : g_strconcat (path, ".bak", NULL);
    gchar *dst_path = is_backup ? g_strconcat (path, ".bak", NULL) : g_strdup (path);

    GFile *src = g_file_new_for_path (src_path);
    GFile *dst = g_file_new_for_path (dst_path);

    /* Tighten umask so a freshly-created destination is born 0600 instead of
     * exposing the encrypted blob (and Argon2id salt+params) for the duration
     * of the copy. Restore immediately after. */
    mode_t old_umask = umask (0077);
    gboolean copied = g_file_copy (src, dst, G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL, NULL, &err);
    umask (old_umask);

    if (!copied) {
        g_propagate_prefixed_error (out_err, err, "Couldn't %s: ",
                                    is_backup ? "create the backup" : "restore the backup");
        err = NULL;
    } else {
        /* Belt-and-braces: if the destination already existed with broader
         * perms, g_file_copy preserves them. Force 0600 unconditionally. */
        if (g_chmod (dst_path, 0600) != 0) {
            g_set_error (out_err, generic_error_gquark (), GENERIC_ERRCODE,
                         "Failed to chmod 0600 on %s: %s", dst_path, g_strerror (errno));
            copied = FALSE;
        } else {
            g_print("%s\n", is_backup ? _("Backup copy successfully created.") : _("Backup copy successfully restored."));
        }
    }

    g_free (src_path);
    g_free (dst_path);

    g_object_unref (src);
    g_object_unref (dst);
    return copied;
}


static gboolean
backup_db (const gchar *path,
           GError     **err)
{
    return perform_backup_restore (path, TRUE, err);
}


gchar *
db_copy_to (const gchar *src_path,
            const gchar *dst_path)
{
    g_return_val_if_fail (src_path != NULL, g_strdup (_("Source path is NULL")));
    g_return_val_if_fail (dst_path != NULL, g_strdup (_("Destination path is NULL")));

    GFile *src = g_file_new_for_path (src_path);
    GFile *dst = g_file_new_for_path (dst_path);

    GError *err = NULL;
    mode_t old_umask = umask (0077);
    gboolean copied = g_file_copy (src, dst,
                                   G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS,
                                   NULL, NULL, NULL, &err);
    umask (old_umask);

    gchar *error_msg = NULL;
    if (!copied) {
        error_msg = g_strdup_printf (_("Couldn't copy database: %s"), err->message);
        g_clear_error (&err);
    } else if (g_chmod (dst_path, 0600) != 0) {
        error_msg = g_strdup_printf (_("Failed to chmod 0600 on %s: %s"),
                                     dst_path, g_strerror (errno));
    }

    g_object_unref (src);
    g_object_unref (dst);

    return error_msg;
}


static gboolean
compute_file_digest (const gchar *path,
                     guint8       digest[32],
                     GError     **err)
{
    int fd = path_open_safe_regular_file (path, err);
    if (fd < 0)
        return FALSE;

    struct stat st;
    if (fstat (fd, &st) != 0) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Cannot stat '%s': %s", path, g_strerror (errno));
        close (fd);
        return FALSE;
    }
    if (st.st_size < 0 || st.st_size > G_MAXSIZE) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "File size for '%s' is invalid.", path);
        close (fd);
        return FALSE;
    }

    gsize size = (gsize) st.st_size;
    g_autofree guint8 *buf = g_malloc0 (size > 0 ? size : 1);
    gsize total = 0;
    while (total < size) {
        ssize_t n = read (fd, buf + total, size - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                         "Failed to read '%s': %s", path, g_strerror (errno));
            close (fd);
            return FALSE;
        }
        if (n == 0) {
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                         "Short read while hashing '%s'.", path);
            close (fd);
            return FALSE;
        }
        total += (gsize) n;
    }
    close (fd);

    gcry_md_hash_buffer (GCRY_MD_SHA256, digest, buf, size);
    return TRUE;
}


static gboolean
loaded_file_digest_matches (DatabaseData *db_data,
                            GError      **err)
{
    guint8 current_digest[32];
    if (!compute_file_digest (db_data->db_path, current_digest, err))
        return FALSE;
    if (memcmp (current_digest, db_data->loaded_file_digest, sizeof (current_digest)) != 0) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Database changed on disk after it was loaded; refusing to overwrite newer data.");
        return FALSE;
    }
    return TRUE;
}


static void
cleanup_db_gfile (GFile    *file,
                  gpointer  stream,
                  GError   *err)
{
    g_object_unref (file);
    g_clear_error (&err);

    if (stream != NULL)
        g_object_unref (stream);
}


static void
free_db_resources (gcry_cipher_hd_t  hd,
                   guchar           *derived_key,
                   guchar           *enc_buf,
                   gchar            *dec_buf,
                   DbHeaderData_v1  *header_data_v1,
                   DbHeaderData_v2  *header_data_v2)
{
    g_free (enc_buf);
    g_free (header_data_v1);
    g_free (header_data_v2);

    if (hd != NULL)
        gcry_cipher_close (hd);
    // explicit_bzero before gcry_free as belt-and-braces: gcry_free is
    // documented to wipe, but failing closed against any future regression
    // in libgcrypt is cheap insurance for the master key.
    if (derived_key != NULL) {
        explicit_bzero (derived_key, ARGON2ID_KEYLEN);
        gcry_free (derived_key);
    }
    if (dec_buf != NULL)
        gcry_free (dec_buf);
}
