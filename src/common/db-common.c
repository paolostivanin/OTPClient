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
#include "gquarks.h"
#include "db-common.h"
#include "file-size.h"


static gint32    get_db_version     (const gchar      *db_path);

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

static gchar    *decrypt_db         (DatabaseData     *db_data,
                                     GError          **err);

static void      encrypt_db         (DatabaseData     *db_data,
                                     GError          **err);

static void      add_to_json        (gpointer          list_elem,
                                     gpointer          json_array);

static void      backup_db          (const gchar      *path);

static void      restore_db         (const gchar      *path);

static void      cleanup_db_gfile   (GFile            *file,
                                     gpointer          stream,
                                     GError           *err);

static void      free_db_resources  (gcry_cipher_hd_t  hd,
                                     guchar           *derived_key,
                                     guchar           *enc_buf,
                                     gchar            *dec_buf,
                                     DbHeaderData_v1  *header_data_v1,
                                     DbHeaderData_v2  *header_data_v2);


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
    db_data->has_cached_key = FALSE;
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

    db_data->current_db_version = get_db_version (db_data->db_path);

    gchar *in_memory_json = decrypt_db (db_data, err);
    g_return_if_fail (err == NULL || *err == NULL);

    json_error_t jerr;
    db_data->in_memory_json_data = json_loads (in_memory_json, 0, &jerr);
    gcry_free (in_memory_json);
    if (db_data->in_memory_json_data == NULL) {
        g_set_error (err, memlock_error_gquark(), MEMLOCK_ERRCODE,
                     "Error while loading json data: %s", jerr.text);
        return;
    }

    if (db_data->current_db_version < 2) {
        update_db (db_data, err);
        g_return_if_fail (err == NULL || *err == NULL);

        if (db_data->in_memory_json_data != NULL) {
            json_decref (db_data->in_memory_json_data);
        }
        g_slist_free_full (db_data->objects_hash, g_free);
        db_data->objects_hash = NULL;

        in_memory_json = decrypt_db (db_data, err);
        g_return_if_fail (err == NULL || *err == NULL);

        db_data->in_memory_json_data = json_loads (in_memory_json, 0, &jerr);
        gcry_free (in_memory_json);
        if (db_data->in_memory_json_data == NULL) {
            g_set_error (err, memlock_error_gquark(), MEMLOCK_ERRCODE,
                         "Error while loading json data: %s", jerr.text);
            return;
        }
    }

    gsize index;
    json_t *obj;
    json_array_foreach (db_data->in_memory_json_data, index, obj) {
        guint32 hash = json_object_get_hash (obj);
        db_data->objects_hash = g_slist_append (db_data->objects_hash, g_memdup2 (&hash, sizeof (guint32)));
    }

    // Opportunistic KDF byte-length migration: if decrypt only succeeded with
    // the legacy g_utf8_strlen length, silently re-encrypt now with the
    // corrected strlen length. encrypt_db clears the flag on success.
    if (db_data->needs_legacy_kdf_migration) {
        g_message ("Migrating database to corrected KDF password byte length.");
        update_db (db_data, err);
    }
}


void
update_db (DatabaseData  *db_data,
           GError       **err)
{
    g_return_if_fail (err == NULL || *err == NULL);

    gboolean first_run = (db_data->in_memory_json_data == NULL) ? TRUE : FALSE;
    if (first_run == TRUE) {
        db_data->in_memory_json_data = json_array ();
        // we need some default values for the first run
        db_data->argon2id_iter = ARGON2ID_DEFAULT_ITER;
        db_data->argon2id_memcost = ARGON2ID_DEFAULT_MC;
        db_data->argon2id_parallelism = ARGON2ID_DEFAULT_PARAL;
    } else {
        // database is backed-up only if this is not the first run
        backup_db (db_data->db_path);
    }

    if (db_data->data_to_add != NULL) {
        g_slist_foreach (db_data->data_to_add, add_to_json, db_data->in_memory_json_data);
    }

    encrypt_db (db_data, err);
    if (err != NULL && *err != NULL) {
        if (!first_run) {
            g_printerr ("Encrypting the new data failed, restoring the original copy...\n");
            restore_db (db_data->db_path);
        } else {
            g_printerr ("Couldn't update the database (encrypt_db failed)\n");
            if (g_file_test (db_data->db_path, G_FILE_TEST_EXISTS)) {
                if (g_unlink (db_data->db_path) == -1) {
                    g_printerr ("%s\n", _("Error while unlinking the file."));
                }
            }
        }
    } else {
        // database must be backed-up both before and after the update
        backup_db (db_data->db_path);
    }
}


void
reload_db (DatabaseData  *db_data,
           GError       **err)
{
    if (db_data->in_memory_json_data != NULL) {
        json_decref (db_data->in_memory_json_data);
    }

    g_slist_free_full (db_data->objects_hash, g_free);
    db_data->objects_hash = NULL;

    load_db (db_data, err);
}


void
add_otps_to_db_ex (GSList       *otps,
                   DatabaseData *db_data,
                   guint        *added_out,
                   guint        *skipped_out)
{
    json_t *obj;
    guint added = 0, skipped = 0;
    guint list_len = g_slist_length (otps);
    for (guint i = 0; i < list_len; i++) {
        otp_t *otp = g_slist_nth_data (otps, i);
        obj = build_json_obj (otp->type, otp->account_name, otp->issuer, otp->secret, otp->digits, otp->algo, otp->period, otp->counter, otp->group);
        guint32 hash = json_object_get_hash (obj);
        if (g_slist_find_custom (db_data->objects_hash, GUINT_TO_POINTER((guint)hash), check_duplicate) == NULL) {
            db_data->objects_hash = g_slist_append (db_data->objects_hash, g_memdup2 (&hash, sizeof (guint32)));
            db_data->data_to_add = g_slist_append (db_data->data_to_add, obj);
            added++;
        } else {
            skipped++;
        }
    }
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
get_db_version (const gchar *db_path)
{
    GError *err = NULL;
    GFile *in_file = g_file_new_for_path (db_path);
    GFileInputStream *in_stream = g_file_read (in_file, NULL, &err);
    if (!in_stream) {
        g_printerr ("%s\n", err->message);
        cleanup_db_gfile (in_file, NULL, err);
        return -1;
    }

    gchar *header_name = g_malloc0 (g_utf8_strlen (DB_HEADER_NAME, -1) + 1);
    if (g_input_stream_read (G_INPUT_STREAM(in_stream), header_name, g_utf8_strlen (DB_HEADER_NAME, -1), NULL, &err) == -1) {
        g_printerr ("%s\n", err->message);
        g_free (header_name);
        cleanup_db_gfile (in_file, in_stream, err);
        return -1;
    }

    gint32 version = (g_strcmp0 (DB_HEADER_NAME, header_name) == 0) ? DB_VERSION : 1;
    g_free (header_name);

    return version;
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
        // Cache hit: same salt as the last derive => same key (password and
        // KDF params haven't changed, since the cache is invalidated on
        // password change). Return a fresh copy so callers can free it.
        if (!use_legacy_length
            && db_data->has_cached_key
            && db_data->cached_derived_key != NULL
            && memcmp (db_data->cached_salt, salt, KDF_SALT_SIZE) == 0) {
            derived_key = gcry_malloc_secure (ARGON2ID_KEYLEN);
            if (derived_key == NULL) {
                g_set_error (err, secmem_alloc_error_gquark (), SECMEM_ALLOC_ERRCODE, "Error while allocating secure memory.");
                return NULL;
            }
            memcpy (derived_key, db_data->cached_derived_key, ARGON2ID_KEYLEN);
            return derived_key;
        }

        derived_key = gcry_malloc_secure (ARGON2ID_KEYLEN);
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

        // Populate cache only on the corrected-length path; the legacy retry
        // is for one-shot read of pre-fix files and shouldn't poison the cache.
        if (!use_legacy_length) {
            if (db_data->cached_derived_key == NULL)
                db_data->cached_derived_key = gcry_malloc_secure (ARGON2ID_KEYLEN);
            if (db_data->cached_derived_key != NULL) {
                memcpy (db_data->cached_derived_key, derived_key, ARGON2ID_KEYLEN);
                memcpy (db_data->cached_salt, salt, KDF_SALT_SIZE);
                db_data->has_cached_key = TRUE;
            }
        }
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
        gcry_free (derived_key);
        return NULL;
    }

    if (gcry_cipher_authenticate (hd, header_data, sizeof(DbHeaderData_v2)) != GPG_ERR_NO_ERROR) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Error while processing the authenticated data.");
        gcry_cipher_close (hd);
        gcry_free (derived_key);
        return NULL;
    }

    gchar *dec_buf = gcry_calloc_secure (enc_buf_size, 1);
    if (!dec_buf) {
        g_set_error (err, secmem_alloc_error_gquark (), SECMEM_ALLOC_ERRCODE, "Error while allocating secure memory.");
        gcry_cipher_close (hd);
        gcry_free (derived_key);
        return NULL;
    }

    if (gcry_cipher_decrypt (hd, dec_buf, enc_buf_size, enc_buf, enc_buf_size) != GPG_ERR_NO_ERROR) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Error while decrypting the data.");
        gcry_cipher_close (hd);
        gcry_free (derived_key);
        gcry_free (dec_buf);
        return NULL;
    }

    if (gcry_err_code (gcry_cipher_checktag (hd, tag, TAG_SIZE)) == GPG_ERR_CHECKSUM) {
        g_set_error (err, bad_tag_gquark (), BAD_TAG_ERRCODE, "The tag doesn't match. Either the password is wrong or the file is corrupted.");
        gcry_cipher_close (hd);
        gcry_free (derived_key);
        gcry_free (dec_buf);
        return NULL;
    }

    gcry_cipher_close (hd);
    gcry_free (derived_key);
    return dec_buf;
}


static gchar *
decrypt_db (DatabaseData *db_data,
            GError      **err)
{
    g_return_val_if_fail (err == NULL || *err == NULL, NULL);

    GFile *in_file = g_file_new_for_path (db_data->db_path);
    GFileInputStream *in_stream = g_file_read (in_file, NULL, NULL);
    if (!in_stream) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed to read the database file.");
        g_object_unref (in_file);
        return NULL;
    }

    DbHeaderData_v1 *header_data_v1 = g_new0 (DbHeaderData_v1, 1);
    DbHeaderData_v2 *header_data_v2 = g_new0 (DbHeaderData_v2, 1);
    goffset header_data_size = (db_data->current_db_version >= 2) ? sizeof(DbHeaderData_v2) : sizeof(DbHeaderData_v1);

    gssize res;
    if (db_data->current_db_version >= 2) {
        res = g_input_stream_read (G_INPUT_STREAM(in_stream), header_data_v2, header_data_size, NULL, NULL);
        db_data->argon2id_iter = header_data_v2->argon2id_iter;
        db_data->argon2id_memcost = header_data_v2->argon2id_memcost;
        db_data->argon2id_parallelism = header_data_v2->argon2id_parallelism;
    } else {
        res = g_input_stream_read (G_INPUT_STREAM(in_stream), header_data_v1, header_data_size, NULL, NULL);
        // when decrypting v1 db, we need to set some default values for the next re-encryption
        db_data->argon2id_iter = ARGON2ID_DEFAULT_ITER;
        db_data->argon2id_memcost = ARGON2ID_DEFAULT_MC;
        db_data->argon2id_parallelism = ARGON2ID_DEFAULT_PARAL;
    }
    if (res == -1) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed to read the header data.");
        cleanup_db_gfile (in_file, in_stream, NULL);
        free_db_resources(NULL, NULL, NULL, NULL, header_data_v1, header_data_v2);
        return NULL;
    }

    if (db_data->current_db_version >= 2) {
        if (db_data->argon2id_iter < ARGON2ID_MIN_ITER || db_data->argon2id_iter > ARGON2ID_MAX_ITER ||
            db_data->argon2id_memcost < ARGON2ID_MIN_MC || db_data->argon2id_memcost > ARGON2ID_MAX_MC ||
            db_data->argon2id_parallelism < ARGON2ID_MIN_PARAL || db_data->argon2id_parallelism > ARGON2ID_MAX_PARAL) {
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                         "Database header contains out-of-range Argon2id parameters "
                         "(iter=%d, memcost=%d KiB, parallelism=%d). Refusing to open: the file may be tampered or corrupted.",
                         db_data->argon2id_iter, db_data->argon2id_memcost, db_data->argon2id_parallelism);
            cleanup_db_gfile (in_file, in_stream, NULL);
            free_db_resources(NULL, NULL, NULL, NULL, header_data_v1, header_data_v2);
            return NULL;
        }
    }

    goffset input_file_size = get_file_size (db_data->db_path);
    guchar tag[TAG_SIZE];
    if (!g_seekable_seek (G_SEEKABLE(in_stream), input_file_size - TAG_SIZE, G_SEEK_SET, NULL, NULL) ||
        g_input_stream_read (G_INPUT_STREAM(in_stream), tag, TAG_SIZE, NULL, NULL) == -1) {
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed to read the stored tag.");
            cleanup_db_gfile (in_file, in_stream, NULL);
            free_db_resources(NULL, NULL, NULL, NULL, header_data_v1, header_data_v2);
            return NULL;
    }

    gsize enc_buf_size = input_file_size - header_data_size - TAG_SIZE;
    guchar *enc_buf = g_malloc0 (enc_buf_size);
    if (!g_seekable_seek (G_SEEKABLE(in_stream), header_data_size, G_SEEK_SET, NULL, NULL) ||
        g_input_stream_read (G_INPUT_STREAM(in_stream), enc_buf, enc_buf_size, NULL, NULL) == -1) {
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed to read the encrypted data.");
            cleanup_db_gfile (in_file, in_stream, NULL);
            free_db_resources(NULL, NULL, enc_buf, NULL, header_data_v1, header_data_v2);
            return NULL;
    }

    g_object_unref (in_stream);
    g_object_unref (in_file);

    if (db_data->current_db_version >= 2) {
        // Modern path: try the corrected (strlen) password length first.
        gchar *dec_buf = try_decrypt_v2 (db_data, header_data_v2, enc_buf, enc_buf_size, tag, FALSE, err);

        if (dec_buf == NULL && err != NULL && *err != NULL && (*err)->domain == bad_tag_gquark ()) {
            // BAD_TAG with the corrected length: the DB may have been written
            // with the legacy g_utf8_strlen byte length (which truncated multi-byte
            // passwords). Skip the retry for pure-ASCII passwords where strlen and
            // g_utf8_strlen always agree — the second attempt would just waste a
            // ~150-300ms Argon2id derivation on a guaranteed-identical result.
            gsize byte_len = strlen (db_data->key);
            gsize char_len = (gsize) g_utf8_strlen (db_data->key, -1);
            if (byte_len != char_len) {
                g_clear_error (err);
                dec_buf = try_decrypt_v2 (db_data, header_data_v2, enc_buf, enc_buf_size, tag, TRUE, err);
                if (dec_buf != NULL) {
                    db_data->needs_legacy_kdf_migration = TRUE;
                }
            }
        }

        free_db_resources (NULL, NULL, enc_buf, NULL, header_data_v1, header_data_v2);
        return dec_buf;
    }

    // v1 (PBKDF2) path: always written with the legacy length, so decrypt with
    // that. The caller migrates v1 -> v2 right after, which is when the
    // corrected length starts being used.
    guchar *derived_key = get_db_derived_key (db_data, db_data->current_db_version, header_data_v1->salt, FALSE, err);
    if (derived_key == NULL) {
        free_db_resources (NULL, NULL, enc_buf, NULL, header_data_v1, header_data_v2);
        return NULL;
    }

    gcry_cipher_hd_t hd = open_cipher_and_set_data (derived_key, header_data_v1->iv, IV_SIZE);
    if (!hd) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Error while opening and setting the cipher data.");
        free_db_resources (NULL, derived_key, enc_buf, NULL, header_data_v1, header_data_v2);
        return NULL;
    }

    if (gcry_cipher_authenticate (hd, header_data_v1, header_data_size) != GPG_ERR_NO_ERROR) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Error while processing the authenticated data.");
        free_db_resources (hd, derived_key, enc_buf, NULL, header_data_v1, header_data_v2);
        return NULL;
    }

    gchar *dec_buf = gcry_calloc_secure (enc_buf_size, 1);
    if (!dec_buf) {
        g_set_error (err, secmem_alloc_error_gquark (), SECMEM_ALLOC_ERRCODE, "Error while allocating secure memory.");
        free_db_resources (hd, derived_key, enc_buf, NULL, header_data_v1, header_data_v2);
        return NULL;
    }

    if (gcry_cipher_decrypt (hd, dec_buf, enc_buf_size, enc_buf, enc_buf_size) != GPG_ERR_NO_ERROR) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Error while decrypting the data.");
        free_db_resources (hd, derived_key, enc_buf, dec_buf, header_data_v1, header_data_v2);
        return NULL;
    }

    if (gcry_err_code (gcry_cipher_checktag (hd, tag, TAG_SIZE)) == GPG_ERR_CHECKSUM) {
        g_set_error (err, bad_tag_gquark (), BAD_TAG_ERRCODE, "The tag doesn't match. Either the password is wrong or the file is corrupted.");
        free_db_resources (hd, derived_key, enc_buf, dec_buf, header_data_v1, header_data_v2);
        return NULL;
    }

    free_db_resources (hd, derived_key, enc_buf, NULL, header_data_v1, header_data_v2);

    return dec_buf;
}


static void
encrypt_db (DatabaseData *db_data,
            GError      **err)
{
    g_return_if_fail (err == NULL || *err == NULL);

    DbHeaderData_v2 *header_data = g_new0 (DbHeaderData_v2, 1);

    memcpy (header_data->header_name, DB_HEADER_NAME, DB_HEADER_NAME_LEN);
    header_data->db_version = DB_VERSION;
    gcry_create_nonce (header_data->iv, IV_SIZE);
    // Reuse the previously-derived salt when we have a cached key so the KDF
    // step is a memcpy instead of a 150 ms Argon2id derivation. The per-save
    // random IV above guarantees AES-GCM nonce uniqueness independently of
    // salt reuse. db_invalidate_kdf_cache() is called on password change to
    // force a fresh salt + key on the next save.
    if (db_data->has_cached_key)
        memcpy (header_data->salt, db_data->cached_salt, KDF_SALT_SIZE);
    else
        gcry_create_nonce (header_data->salt, KDF_SALT_SIZE);
    header_data->argon2id_iter = db_data->argon2id_iter;
    header_data->argon2id_memcost = db_data->argon2id_memcost;
    header_data->argon2id_parallelism = db_data->argon2id_parallelism;

    GFile *out_file = g_file_new_for_path (db_data->db_path);
    GFileOutputStream *out_stream = g_file_replace (out_file, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION | G_FILE_CREATE_PRIVATE, NULL, NULL);
    if (out_stream == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed to replace existing file");
        g_object_unref (out_file);
        g_free (header_data);
        return;
    }

    if (g_output_stream_write (G_OUTPUT_STREAM (out_stream), header_data, sizeof(DbHeaderData_v2), NULL, NULL) == -1) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed to write the header data to file");
        cleanup_db_gfile (out_file, out_stream, NULL);
        g_free (header_data);
        return;
    }

    // encrypt_db unconditionally uses the corrected (strlen) password byte length.
    // The legacy g_utf8_strlen length is only used on the decrypt retry path
    // when reading older databases (see decrypt_db / try_decrypt_v2).
    guchar *derived_key = get_db_derived_key (db_data, header_data->db_version, header_data->salt, FALSE, err);
    if (derived_key == NULL) {
        cleanup_db_gfile (out_file, out_stream, NULL);
        g_free (header_data);
        return;
    }

    gchar *in_memory_dumped_data = json_dumps (db_data->in_memory_json_data, JSON_COMPACT);
    if (in_memory_dumped_data == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed to serialize the in-memory database.");
        cleanup_db_gfile (out_file, out_stream, NULL);
        gcry_free (derived_key);
        g_free (header_data);
        return;
    }
    gsize input_data_len = strlen (in_memory_dumped_data) + 1;
    guchar *enc_buffer = g_malloc0 (input_data_len);

    gcry_cipher_hd_t hd = open_cipher_and_set_data (derived_key, header_data->iv, IV_SIZE);
    if (hd == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed to open the cipher and set the data.");
        cleanup_db_gfile (out_file, out_stream, NULL);
        free_db_resources (NULL, derived_key, enc_buffer, NULL, NULL, header_data);
        return;
    }

    if (gcry_cipher_authenticate (hd, header_data, sizeof(DbHeaderData_v2)) != GPG_ERR_NO_ERROR) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Error while processing the authenticated data");
        cleanup_db_gfile (out_file, out_stream, NULL);
        free_db_resources (hd, derived_key, enc_buffer, NULL, NULL, header_data);
        return;
    }

    if (gcry_cipher_encrypt (hd, enc_buffer, input_data_len, in_memory_dumped_data, input_data_len) != GPG_ERR_NO_ERROR) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Error while encrypting the data.");
        cleanup_db_gfile (out_file, out_stream, NULL);
        free_db_resources (hd, derived_key, enc_buffer, NULL, NULL, header_data);
        return;
    }
    gcry_free (in_memory_dumped_data);

    if (g_output_stream_write (G_OUTPUT_STREAM(out_stream), enc_buffer, input_data_len, NULL, NULL) == -1) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed while writing encrypted buffer to file");
        cleanup_db_gfile (out_file, out_stream, NULL);
        free_db_resources (hd, derived_key, enc_buffer, NULL, NULL, header_data);
        return;
    }

    guchar tag[TAG_SIZE];
    if (gcry_cipher_gettag (hd, tag, TAG_SIZE) != GPG_ERR_NO_ERROR) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Error while getting the tag.");
        cleanup_db_gfile (out_file, out_stream, NULL);
        free_db_resources (hd, derived_key, enc_buffer, NULL, NULL, header_data);
        return;
    }
    if (g_output_stream_write (G_OUTPUT_STREAM(out_stream), tag, TAG_SIZE, NULL, NULL) == -1) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed while writing tag data to file");
        cleanup_db_gfile (out_file, out_stream, NULL);
        free_db_resources (hd, derived_key, enc_buffer, NULL, NULL, header_data);
        return;
    }

    free_db_resources (hd, derived_key, enc_buffer, NULL, NULL, header_data);

    /* Flush and sync to disk before cleanup to prevent data loss on power failure */
    if (!g_output_stream_close (G_OUTPUT_STREAM (out_stream), NULL, NULL)) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed to flush database file to disk");
    }
    int fd = g_open (db_data->db_path, O_RDONLY, 0);
    if (fd >= 0) {
        fsync (fd);
        close (fd);
    }

    cleanup_db_gfile (out_file, out_stream, NULL);

    db_data->current_db_version = DB_VERSION;
    // The just-written file uses the corrected password byte length.
    db_data->needs_legacy_kdf_migration = FALSE;
}


static void
add_to_json (gpointer list_elem,
             gpointer json_array)
{
    json_array_append (json_array, json_deep_copy (list_elem));
}


static void
perform_backup_restore (const gchar *path,
                        gboolean     is_backup)
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
        g_printerr ("Couldn't %s: %s\n", is_backup ? "create the backup" : "restore the backup", err->message);
        g_clear_error (&err);
    } else {
        /* Belt-and-braces: if the destination already existed with broader
         * perms, g_file_copy preserves them. Force 0600 unconditionally. */
        if (g_chmod (dst_path, 0600) != 0) {
            g_warning ("Failed to chmod 0600 on %s: %s", dst_path, g_strerror (errno));
        }
        g_print("%s\n", is_backup ? _("Backup copy successfully created.") : _("Backup copy successfully restored."));
    }

    g_free (src_path);
    g_free (dst_path);

    g_object_unref (src);
    g_object_unref (dst);
}


static void
backup_db (const gchar *path)
{
    perform_backup_restore (path, TRUE);
}


static void
restore_db (const gchar *path)
{
    perform_backup_restore (path, FALSE);
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
    if (derived_key != NULL)
        gcry_free (derived_key);
    if (dec_buf != NULL)
        gcry_free (dec_buf);
}
