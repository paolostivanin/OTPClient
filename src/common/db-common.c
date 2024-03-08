#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gcrypt.h>
#include <jansson.h>
#include <glib/gstdio.h>
#include "gquarks.h"
#include "db-common.h"
#include "file-size.h"

static gchar    *decrypt_db  (const gchar   *db_path,
                              const gchar   *password);

static gpointer  encrypt_db  (const gchar   *db_path,
                              const gchar   *in_memory_json,
                              const gchar   *password,
                              GError       **err);

static void      add_to_json (gpointer       list_elem,
                              gpointer       json_array);

static void      backup_db   (const gchar   *path);

static void      restore_db  (const gchar   *path);


void
load_db (DatabaseData    *db_data,
         GError         **err)
{
    if (!g_file_test (db_data->db_path, G_FILE_TEST_EXISTS)) {
        g_set_error (err, missing_file_gquark (), MISSING_FILE_CODE, "Missing database file");
        db_data->json_data = NULL;
        return;
    }

    gchar *in_memory_json = decrypt_db (db_data->db_path, db_data->key);
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
    db_data->json_data = json_loads (in_memory_json, 0, &jerr);
    gcry_free (in_memory_json);
    if (db_data->json_data == NULL) {
        gchar *msg = g_strconcat ("Error while loading json data: ", jerr.text, NULL);
        g_set_error (err, memlock_error_gquark(), MEMLOCK_ERRCODE, "%s", msg);
        return;
    }

    gsize index;
    json_t *obj;
    json_array_foreach (db_data->json_data, index, obj) {
        guint32 hash = json_object_get_hash (obj);
        db_data->objects_hash = g_slist_append (db_data->objects_hash, g_memdup2 (&hash, sizeof (guint32)));
    }
}


void
update_db (DatabaseData  *db_data,
           GError       **err)
{
    gboolean first_run = (db_data->json_data == NULL) ? TRUE : FALSE;
    if (first_run == TRUE) {
        db_data->json_data = json_array ();
    } else {
        // database is backed-up only if this is not the first run
        backup_db (db_data->db_path);
    }

    g_slist_foreach (db_data->data_to_add, add_to_json, db_data->json_data);

    gchar *plain_data = json_dumps (db_data->json_data, JSON_COMPACT);

    if (encrypt_db (db_data->db_path, plain_data, db_data->key, err) != NULL) {
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

    gcry_free (plain_data);
}


void
reload_db (DatabaseData  *db_data,
           GError       **err)
{
    if (db_data->json_data != NULL) {
        json_decref (db_data->json_data);
    }

    g_slist_free_full (db_data->objects_hash, g_free);
    db_data->objects_hash = NULL;

    load_db (db_data, err);
}


guchar *
get_db_derived_key (const gchar    *pwd,
                    DbHeaderData   *header_data)
{
    gsize key_len = gcry_cipher_get_algo_keylen (GCRY_CIPHER_AES256);
    gsize pwd_len = g_utf8_strlen (pwd, -1) + 1;

    guchar *derived_key = gcry_malloc_secure (key_len);
    if (derived_key == NULL) {
        g_printerr ("%s\n", _("Couldn't allocate the needed secure memory."));
        return SECURE_MEMORY_ALLOC_ERR;
    }

    gpg_error_t ret = gcry_kdf_derive (pwd, pwd_len, GCRY_KDF_PBKDF2, GCRY_MD_SHA512, header_data->salt, KDF_SALT_SIZE, KDF_ITERATIONS, key_len, derived_key);
    if (ret != 0) {
        gcry_free (derived_key);
        g_printerr ("%s\n", _("Error during key derivation."));
        return KEY_DERIV_ERR;
    }
    return derived_key;
}


void
cleanup_db_gfile (GFile    *file,
                  gpointer  stream,
                  GError   *err)
{
    g_object_unref (file);
    g_clear_error (&err);

    if (stream != NULL)
        g_object_unref (stream);
}


void
free_db_resources (gcry_cipher_hd_t  hd,
                   guchar           *derived_key,
                   guchar           *enc_buf,
                   gchar            *dec_buf,
                   DbHeaderData     *header_data)
{
    g_free (enc_buf);
    g_free (header_data);

    if (hd != NULL)
        gcry_cipher_close (hd);
    if (derived_key != NULL)
        gcry_free (derived_key);
    if (dec_buf != NULL)
        gcry_free (dec_buf);
}


static gchar *
decrypt_db (const gchar *db_path,
            const gchar *password)
{
    GError *err = NULL;
    DbHeaderData *header_data = g_new0 (DbHeaderData, 1);

    goffset input_file_size = get_file_size (db_path);

    GFile *in_file = g_file_new_for_path (db_path);
    GFileInputStream *in_stream = g_file_read (in_file, NULL, &err);
    if (err != NULL) {
        g_printerr ("%s\n", err->message);
        cleanup_db_gfile (in_file, NULL, err);
        g_free (header_data);
        return GENERIC_ERROR;
    }
    if (g_input_stream_read (G_INPUT_STREAM (in_stream), header_data, sizeof (DbHeaderData), NULL, &err) == -1) {
        g_printerr ("%s\n", err->message);
        cleanup_db_gfile (in_file, in_stream, err);
        g_free (header_data);
        return GENERIC_ERROR;
    }

    guchar tag[TAG_SIZE];
    if (!g_seekable_seek (G_SEEKABLE (in_stream), input_file_size - TAG_SIZE, G_SEEK_SET, NULL, &err)) {
        g_printerr ("%s\n", err->message);
        cleanup_db_gfile (in_file, in_stream, err);
        g_free (header_data);
        return GENERIC_ERROR;
    }
    if (g_input_stream_read (G_INPUT_STREAM (in_stream), tag, TAG_SIZE, NULL, &err) == -1) {
        g_printerr ("%s\n", err->message);
        cleanup_db_gfile (in_file, in_stream, err);
        g_free (header_data);
        return GENERIC_ERROR;
    }

    gsize enc_buf_size = input_file_size - sizeof (DbHeaderData) - TAG_SIZE;
    guchar *enc_buf = g_malloc0 (enc_buf_size);

    if (!g_seekable_seek (G_SEEKABLE (in_stream), sizeof (DbHeaderData), G_SEEK_SET, NULL, &err)) {
        g_printerr ("%s\n", err->message);
        cleanup_db_gfile (in_file, in_stream, err);

        return GENERIC_ERROR;
    }
    if (g_input_stream_read (G_INPUT_STREAM (in_stream), enc_buf, enc_buf_size, NULL, &err) == -1) {
        g_printerr ("%s\n", err->message);
        cleanup_db_gfile (in_file, in_stream, err);
        free_db_resources (NULL, NULL, enc_buf, NULL, header_data);
        return GENERIC_ERROR;
    }
    g_object_unref (in_stream);
    g_object_unref (in_file);

    guchar *derived_key = get_db_derived_key (password, header_data);
    if (derived_key == SECURE_MEMORY_ALLOC_ERR || derived_key == KEY_DERIV_ERR) {
        free_db_resources (NULL, NULL, enc_buf, NULL, header_data);
        return (gpointer)derived_key;
    }

    gcry_cipher_hd_t hd = open_cipher_and_set_data (derived_key, header_data->iv, IV_SIZE);
    if (hd == NULL) {
        free_db_resources (NULL, derived_key, enc_buf, NULL, header_data);
        return GENERIC_ERROR;
    }

    gpg_error_t gpg_err = gcry_cipher_authenticate (hd, header_data, sizeof (DbHeaderData));
    if (gpg_err) {
        g_printerr ("%s\n", _("Error while processing the authenticated data."));
        free_db_resources (hd, derived_key, enc_buf, NULL, header_data);
        return GENERIC_ERROR;
    }

    gchar *dec_buf = gcry_calloc_secure (enc_buf_size, 1);
    if (dec_buf == NULL) {
        g_printerr ("%s\n", _("Error while allocating secure memory."));
        free_db_resources (hd, derived_key, enc_buf, NULL, header_data);
        return GENERIC_ERROR;
    }
    gpg_err = gcry_cipher_decrypt (hd, dec_buf, enc_buf_size, enc_buf, enc_buf_size);
    if (gpg_err) {
        g_printerr ("%s\n", _("Error while decrypting the data."));
        free_db_resources (hd, derived_key, enc_buf, dec_buf, header_data);
        return GENERIC_ERROR;
    }
    if (gcry_err_code (gcry_cipher_checktag (hd, tag, TAG_SIZE)) == GPG_ERR_CHECKSUM) {
        free_db_resources (hd, derived_key, enc_buf, dec_buf, header_data);
        return TAG_MISMATCH;
    }

    free_db_resources (hd, derived_key, enc_buf, NULL, header_data);

    return dec_buf;
}


static void
add_to_json (gpointer list_elem,
             gpointer json_array)
{
    json_array_append (json_array, json_deep_copy (list_elem));
}


static gpointer
encrypt_db (const gchar  *db_path,
            const gchar  *in_memory_json,
            const gchar  *password,
            GError      **err)
{
    GError *local_err = NULL;
    DbHeaderData *header_data = g_new0 (DbHeaderData, 1);

    gcry_create_nonce (header_data->iv, IV_SIZE);
    gcry_create_nonce (header_data->salt, KDF_SALT_SIZE);

    GFile *out_file = g_file_new_for_path (db_path);
    GFileOutputStream *out_stream = g_file_replace (out_file, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION, NULL, &local_err);
    if (local_err != NULL) {
        g_printerr ("%s\n", local_err->message);
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed to replace existing file");
        cleanup_db_gfile (out_file, NULL, local_err);
        g_free (header_data);
        return GENERIC_ERROR;
    }
    if (g_output_stream_write (G_OUTPUT_STREAM (out_stream), header_data, sizeof (DbHeaderData), NULL, &local_err) == -1) {
        g_printerr ("%s\n", local_err->message);
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed while writing header data to file");
        cleanup_db_gfile (out_file, out_stream, local_err);
        g_free (header_data);
        return GENERIC_ERROR;
    }

    guchar *derived_key = get_db_derived_key (password, header_data);
    if (derived_key == SECURE_MEMORY_ALLOC_ERR || derived_key == KEY_DERIV_ERR) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed to derive key.\nPlease check <a href=\"https://github.com/paolostivanin/OTPClient/wiki/Secure-Memory-Limitations\">Secure Memory wiki page</a>");
        cleanup_db_gfile (out_file, out_stream, local_err);
        g_free (header_data);
        return (gpointer)derived_key;
    }

    gsize input_data_len = strlen (in_memory_json) + 1;
    guchar *enc_buffer = g_malloc0 (input_data_len);

    gcry_cipher_hd_t hd = open_cipher_and_set_data (derived_key, header_data->iv, IV_SIZE);
    if (hd == NULL) {
        cleanup_db_gfile (out_file, out_stream, local_err);
        free_db_resources (NULL, derived_key, enc_buffer, NULL, header_data);
        return NULL;
    }

    gpg_error_t gpg_err = gcry_cipher_authenticate (hd, header_data, sizeof (DbHeaderData));
    if (gpg_err) {
        g_printerr ("%s\n", _("Error while processing the authenticated data."));
        cleanup_db_gfile (out_file, out_stream, local_err);
        free_db_resources (hd, derived_key, enc_buffer, NULL, header_data);
        return GENERIC_ERROR;
    }
    gpg_err = gcry_cipher_encrypt (hd, enc_buffer, input_data_len, in_memory_json, input_data_len);
    if (gpg_err) {
        g_printerr ("%s\n", _("Error while encrypting the data."));
        cleanup_db_gfile (out_file, out_stream, local_err);
        free_db_resources (hd, derived_key, enc_buffer, NULL, header_data);
        return GENERIC_ERROR;
    }

    guchar tag[TAG_SIZE];
    gpg_err = gcry_cipher_gettag (hd, tag, TAG_SIZE); //append tag to outfile
    if (gpg_err) {
        g_printerr ("%s\n", _("Error while getting the tag."));
        cleanup_db_gfile (out_file, out_stream, local_err);
        free_db_resources (hd, derived_key, enc_buffer, NULL, header_data);
        return GENERIC_ERROR;
    }

    if (g_output_stream_write (G_OUTPUT_STREAM(out_stream), enc_buffer, input_data_len, NULL, &local_err) == -1) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed while writing encrypted buffer to file");
        cleanup_db_gfile (out_file, out_stream, local_err);
        free_db_resources (hd, derived_key, enc_buffer, NULL, header_data);
        return GENERIC_ERROR;
    }
    if (g_output_stream_write (G_OUTPUT_STREAM(out_stream), tag, TAG_SIZE, NULL, &local_err) == -1) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Failed while writing tag data to file");
        cleanup_db_gfile (out_file, out_stream, local_err);
        free_db_resources (hd, derived_key, enc_buffer, NULL, header_data);
        return GENERIC_ERROR;
    }

    free_db_resources (hd, derived_key, enc_buffer, NULL, header_data);
    cleanup_db_gfile (out_file, out_stream, NULL);

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