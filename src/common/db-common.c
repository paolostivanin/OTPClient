#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gcrypt.h>
#include <jansson.h>
#include <glib/gstdio.h>
#include <string.h>
#include "glibconfig.h"
#include "gquarks.h"
#include "db-common.h"
#include "file-size.h"


static gint32    get_db_version     (const gchar      *db_path);

static guchar   *get_db_derived_key (const gchar      *pwd,
                                     gint32            db_version,
                                     const guint8     *salt);

static gchar    *decrypt_db         (DatabaseData     *db_data);

static gpointer  encrypt_db         (DatabaseData     *db_data,
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
load_db (DatabaseData    *db_data,
         GError         **err)
{
    if (!g_file_test (db_data->db_path, G_FILE_TEST_EXISTS)) {
        g_set_error (err, missing_file_gquark (), MISSING_FILE_CODE, "Missing database file");
        db_data->in_memory_json_data = NULL;
        return;
    }

    db_data->current_db_version = get_db_version (db_data->db_path);

    gchar *in_memory_json = decrypt_db (db_data);
    if (in_memory_json == TAG_MISMATCH) {
        g_set_error (err, bad_tag_gquark (), BAD_TAG_ERRCODE, "Either the file is corrupted or the password is wrong");
        return;
    } else if (in_memory_json == KEY_DERIV_ERR) {
        g_set_error (err, key_deriv_gquark (), KEY_DERIVATION_ERRCODE, "Error during key derivation");
        return;
    } else if (in_memory_json == GENERIC_ERROR) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "An error occurred, please check stderr");
        return;
    }

    json_error_t jerr;
    db_data->in_memory_json_data = json_loads (in_memory_json, 0, &jerr);
    gcry_free (in_memory_json);
    if (db_data->in_memory_json_data == NULL) {
        gchar *msg = g_strconcat ("Error while loading json data: ", jerr.text, NULL);
        g_set_error (err, memlock_error_gquark(), MEMLOCK_ERRCODE, "%s", msg);
        return;
    }

    gsize index;
    json_t *obj;
    json_array_foreach (db_data->in_memory_json_data, index, obj) {
        guint32 hash = json_object_get_hash (obj);
        db_data->objects_hash = g_slist_append (db_data->objects_hash, g_memdup2 (&hash, sizeof (guint32)));
    }
}


void
update_db (DatabaseData  *db_data,
           GError       **err)
{
    gboolean first_run = (db_data->in_memory_json_data == NULL) ? TRUE : FALSE;
    if (first_run == TRUE) {
        db_data->in_memory_json_data = json_array ();
    } else {
        // database is backed-up only if this is not the first run
        backup_db (db_data->db_path);
    }

    g_slist_foreach (db_data->data_to_add, add_to_json, db_data->in_memory_json_data);

    if (encrypt_db (db_data, err) != NULL) {
        if (!first_run) {
            g_printerr ("Encrypting the new data failed, restoring original copy...\n");
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
add_otps_to_db (GSList       *otps,
                DatabaseData *db_data)
{
    json_t *obj;
    guint list_len = g_slist_length (otps);
    for (guint i = 0; i < list_len; i++) {
        otp_t *otp = g_slist_nth_data (otps, i);
        obj = build_json_obj (otp->type, otp->account_name, otp->issuer, otp->secret, otp->digits, otp->algo, otp->period, otp->counter);
        guint hash = json_object_get_hash (obj);
        if (g_slist_find_custom (db_data->objects_hash, GUINT_TO_POINTER(hash), check_duplicate) == NULL) {
            db_data->objects_hash = g_slist_append (db_data->objects_hash, g_memdup2 (&hash, sizeof (guint)));
            db_data->data_to_add = g_slist_append (db_data->data_to_add, obj);
        } else {
            g_print ("[INFO] Duplicate element not added\n");
        }
    }
}


gint
check_duplicate (gconstpointer data,
                 gconstpointer user_data)
{
    guint list_elem = *(guint *)data;
    if (list_elem == GPOINTER_TO_UINT(user_data)) {
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
        cleanup_db_gfile (in_file, in_stream, err);
        return -1;
    }

    gint32 version = (g_strcmp0 (DB_HEADER_NAME, header_name) == 0) ? DB_VERSION : 1;
    g_free (header_name);

    return version;
}


static guchar *
get_db_derived_key (const gchar  *pwd,
                    gint32        db_version,
                    const guint8 *salt)
{
    guchar *derived_key = NULL;
    if (db_version < 2) {
        gsize key_len = gcry_cipher_get_algo_keylen (GCRY_CIPHER_AES256);

        derived_key = gcry_malloc_secure (key_len);
        if (derived_key == NULL) {
            g_printerr ("%s\n", _("Couldn't allocate the needed secure memory."));
            return SECURE_MEMORY_ALLOC_ERR;
        }

        if (gcry_kdf_derive (pwd, (gsize)g_utf8_strlen (pwd, -1),
                             GCRY_KDF_PBKDF2, GCRY_MD_SHA512,
                             salt, KDF_SALT_SIZE,
                             KDF_ITERATIONS, key_len, derived_key) != GPG_ERR_NO_ERROR) {
            gcry_free (derived_key);
            g_printerr ("%s\n", _("Error during key derivation."));
            return KEY_DERIV_ERR;
        }
    } else {
        derived_key = gcry_malloc_secure (ARGON2ID_KEYLEN);
        const unsigned long params[4] = {ARGON2ID_TAGLEN, ARGON2ID_ITER, ARGON2ID_MEMCOST, ARGON2ID_PARALLELISM};
        gcry_kdf_hd_t hd;
        if (gcry_kdf_open (&hd, GCRY_KDF_ARGON2, GCRY_KDF_ARGON2ID,
                           params, 4,
                           pwd, (gsize)g_utf8_strlen (pwd, -1),
                           salt, KDF_SALT_SIZE,
                           NULL, 0, NULL, 0) != GPG_ERR_NO_ERROR) {
            g_printerr ("Error while opening the KDF handler\n");
            gcry_free (derived_key);
            return NULL;
        }
        if (gcry_kdf_compute (hd, NULL) != GPG_ERR_NO_ERROR) {
            g_printerr ("Error while computing the KDF\n");
            gcry_free (derived_key);
            gcry_kdf_close (hd);
            return NULL;
        }
        if (gcry_kdf_final (hd, ARGON2ID_KEYLEN, derived_key) != GPG_ERR_NO_ERROR) {
            g_printerr ("Error while finalizing the KDF handler\n");
            gcry_free (derived_key);
            gcry_kdf_close (hd);
            return NULL;
        }
        gcry_kdf_close (hd);
    }

    return derived_key;
}


static gchar *
decrypt_db (DatabaseData *db_data)
{
    GError *err = NULL;

    GFile *in_file = g_file_new_for_path (db_data->db_path);
    GFileInputStream *in_stream = g_file_read (in_file, NULL, &err);
    if (!in_stream) {
        g_printerr ("%s\n", err->message);
        cleanup_db_gfile (in_file, NULL, err);
        return GENERIC_ERROR;
    }

    DbHeaderData_v1 *header_data_v1 = g_new0 (DbHeaderData_v1, 1);
    DbHeaderData_v2 *header_data_v2 = g_new0 (DbHeaderData_v2, 1);
    gsize header_data_size = (db_data->current_db_version >= 2) ? sizeof(DbHeaderData_v2) : sizeof(DbHeaderData_v1);

    gssize res;
    if (db_data->current_db_version >= 2) {
        res = g_input_stream_read (G_INPUT_STREAM(in_stream), header_data_v2, header_data_size, NULL, &err);
    } else {
        res = g_input_stream_read (G_INPUT_STREAM(in_stream), header_data_v1, header_data_size, NULL, &err);
    }
    if (res == -1) {
        g_printerr ("%s\n", err->message);
        cleanup_db_gfile (in_file, in_stream, err);
        g_free (header_data_v1);
        g_free (header_data_v2);
        return GENERIC_ERROR;
    }

    goffset input_file_size = get_file_size (db_data->db_path);
    guchar tag[TAG_SIZE];
    if (!g_seekable_seek (G_SEEKABLE(in_stream), input_file_size - TAG_SIZE, G_SEEK_SET, NULL, &err) ||
        g_input_stream_read (G_INPUT_STREAM(in_stream), tag, TAG_SIZE, NULL, &err) == -1) {
        g_printerr("%s\n", err->message);
        cleanup_db_gfile (in_file, in_stream, err);
        g_free (header_data_v1);
        g_free (header_data_v2);
        return GENERIC_ERROR;
    }

    gsize enc_buf_size = input_file_size - header_data_size - TAG_SIZE;
    guchar *enc_buf = g_malloc0 (enc_buf_size);
    if (!g_seekable_seek (G_SEEKABLE(in_stream), header_data_size, G_SEEK_SET, NULL, &err) ||
        g_input_stream_read (G_INPUT_STREAM(in_stream), enc_buf, enc_buf_size, NULL, &err) == -1) {
        g_printerr ("%s\n", err->message);
        cleanup_db_gfile (in_file, in_stream, err);
        free_db_resources(NULL, NULL, enc_buf, NULL, header_data_v1, header_data_v2);
        return GENERIC_ERROR;
    }

    g_object_unref (in_stream);
    g_object_unref (in_file);

    guchar *derived_key = NULL;
    if (db_data->current_db_version >= 2) {
        derived_key = get_db_derived_key (db_data->key, db_data->current_db_version, header_data_v2->salt);
    } else {
        derived_key = get_db_derived_key (db_data->key, db_data->current_db_version, header_data_v1->salt);
    }
    if (derived_key == SECURE_MEMORY_ALLOC_ERR || derived_key == KEY_DERIV_ERR) {
        free_db_resources (NULL, NULL, enc_buf, NULL, header_data_v1, header_data_v2);
        return (gpointer)derived_key;
    }

    gcry_cipher_hd_t hd;
    if (db_data->current_db_version >= 2) {
        hd = open_cipher_and_set_data (derived_key, header_data_v2->iv, IV_SIZE);
    } else {
        hd = open_cipher_and_set_data (derived_key, header_data_v1->iv, IV_SIZE);
    }
    if (!hd) {
        free_db_resources (NULL, derived_key, enc_buf, NULL, header_data_v1, header_data_v2);
        return GENERIC_ERROR;
    }

    gpg_error_t gpg_err;
    if (db_data->current_db_version >= 2) {
        gpg_err = gcry_cipher_authenticate (hd, header_data_v2, header_data_size);
    } else {
        gpg_err = gcry_cipher_authenticate (hd, header_data_v1, header_data_size);
    }
    if (gpg_err != GPG_ERR_NO_ERROR) {
        g_printerr ("%s\n", _("Error while processing the authenticated data."));
        free_db_resources (hd, derived_key, enc_buf, NULL, header_data_v1, header_data_v2);
        return GENERIC_ERROR;
    }

    gchar *dec_buf = gcry_calloc_secure (enc_buf_size, 1);
    if (!dec_buf) {
        g_printerr ("%s\n", _("Error while allocating secure memory."));
        free_db_resources (hd, derived_key, enc_buf, NULL, header_data_v1, header_data_v2);
        return GENERIC_ERROR;
    }

    if (gcry_cipher_decrypt (hd, dec_buf, enc_buf_size, enc_buf, enc_buf_size) != GPG_ERR_NO_ERROR) {
        g_printerr ("%s\n", _("Error while decrypting the data."));
        free_db_resources (hd, derived_key, enc_buf, dec_buf, header_data_v1, header_data_v2);
        return GENERIC_ERROR;
    }

    if (gcry_err_code (gcry_cipher_checktag (hd, tag, TAG_SIZE)) == GPG_ERR_CHECKSUM) {
        free_db_resources (hd, derived_key, enc_buf, dec_buf, header_data_v1, header_data_v2);
        return TAG_MISMATCH;
    }

    free_db_resources (hd, derived_key, enc_buf, NULL, header_data_v1, header_data_v2);

    return dec_buf;
}


static void
add_to_json (gpointer list_elem,
             gpointer json_array)
{
    json_array_append (json_array, json_deep_copy (list_elem));
}


static gpointer
encrypt_db (DatabaseData *db_data,
            GError      **err)
{
    GError *local_err = NULL;
    DbHeaderData_v2 *header_data = g_new0 (DbHeaderData_v2, 1);

    memcpy (header_data->header_name, DB_HEADER_NAME, DB_HEADER_NAME_LEN);
    header_data->db_version = DB_VERSION;
    gcry_create_nonce (header_data->iv, IV_SIZE);
    gcry_create_nonce (header_data->salt, KDF_SALT_SIZE);

    GFile *out_file = g_file_new_for_path (db_data->db_path);
    GFileOutputStream *out_stream = g_file_replace (out_file, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION, NULL, &local_err);
    if (local_err != NULL) {
        g_printerr ("%s\n", local_err->message);
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed to replace existing file");
        cleanup_db_gfile (out_file, NULL, local_err);
        g_free (header_data);
        return GENERIC_ERROR;
    }

    if (g_output_stream_write (G_OUTPUT_STREAM (out_stream), header_data, sizeof(DbHeaderData_v2), NULL, &local_err) == -1) {
        g_printerr ("%s\n", local_err->message);
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed while writing header data to file");
        cleanup_db_gfile (out_file, out_stream, local_err);
        g_free (header_data);
        return GENERIC_ERROR;
    }

    guchar *derived_key = get_db_derived_key (db_data->key, header_data->db_version, header_data->salt);
    if (derived_key == SECURE_MEMORY_ALLOC_ERR || derived_key == KEY_DERIV_ERR) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed to derive key.\nPlease check <a href=\"https://github.com/paolostivanin/OTPClient/wiki/Secure-Memory-Limitations\">Secure Memory wiki page</a>");
        cleanup_db_gfile (out_file, out_stream, local_err);
        g_free (header_data);
        return (gpointer)derived_key;
    }

    gchar *in_memory_dumped_data = json_dumps (db_data->in_memory_json_data, JSON_COMPACT);
    gsize input_data_len = strlen (in_memory_dumped_data) + 1;
    guchar *enc_buffer = g_malloc0 (input_data_len);

    gcry_cipher_hd_t hd = open_cipher_and_set_data (derived_key, header_data->iv, IV_SIZE);
    if (hd == NULL) {
        cleanup_db_gfile (out_file, out_stream, local_err);
        free_db_resources (NULL, derived_key, enc_buffer, NULL, NULL, header_data);
        return NULL;
    }

    if (gcry_cipher_authenticate (hd, header_data, sizeof(DbHeaderData_v2)) != GPG_ERR_NO_ERROR) {
        g_printerr ("%s\n", _("Error while processing the authenticated data."));
        cleanup_db_gfile (out_file, out_stream, local_err);
        free_db_resources (hd, derived_key, enc_buffer, NULL, NULL, header_data);
        return GENERIC_ERROR;
    }

    if (gcry_cipher_encrypt (hd, enc_buffer, input_data_len, in_memory_dumped_data, input_data_len) != GPG_ERR_NO_ERROR) {
        g_printerr ("%s\n", _("Error while encrypting the data."));
        cleanup_db_gfile (out_file, out_stream, local_err);
        free_db_resources (hd, derived_key, enc_buffer, NULL, NULL, header_data);
        return GENERIC_ERROR;
    }
    gcry_free (in_memory_dumped_data);

    if (g_output_stream_write (G_OUTPUT_STREAM(out_stream), enc_buffer, input_data_len, NULL, &local_err) == -1) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed while writing encrypted buffer to file");
        cleanup_db_gfile (out_file, out_stream, local_err);
        free_db_resources (hd, derived_key, enc_buffer, NULL, NULL, header_data);
        return GENERIC_ERROR;
    }

    guchar tag[TAG_SIZE];
    if (gcry_cipher_gettag (hd, tag, TAG_SIZE) != GPG_ERR_NO_ERROR) {
        g_printerr ("%s\n", _("Error while getting the tag."));
        cleanup_db_gfile (out_file, out_stream, local_err);
        free_db_resources (hd, derived_key, enc_buffer, NULL, NULL, header_data);
        return GENERIC_ERROR;
    }
    if (g_output_stream_write (G_OUTPUT_STREAM(out_stream), tag, TAG_SIZE, NULL, &local_err) == -1) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed while writing tag data to file");
        cleanup_db_gfile (out_file, out_stream, local_err);
        free_db_resources (hd, derived_key, enc_buffer, NULL, NULL, header_data);
        return GENERIC_ERROR;
    }

    free_db_resources (hd, derived_key, enc_buffer, NULL, NULL, header_data);
    cleanup_db_gfile (out_file, out_stream, NULL);

    db_data->current_db_version = DB_VERSION;

    return NULL;
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

    g_free (src_path);
    g_free (dst_path);

    if (!g_file_copy (src, dst, G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL, NULL, &err)) {
        g_printerr ("Couldn't %s: %s\n", is_backup ? "create the backup" : "restore the backup", err->message);
        g_clear_error (&err);
    } else {
        g_print("%s\n", is_backup ? _("Backup copy successfully created.") : _("Backup copy successfully restored."));
    }

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
