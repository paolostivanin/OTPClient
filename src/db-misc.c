#include <gtk/gtk.h>
#include <gcrypt.h>
#include <json-glib/json-glib.h>
#include "db-misc.h"
#include "otpclient.h"
#include "file-size.h"
#include "gquarks.h"

static gpointer encrypt_db (const gchar *in_memory_json, const gchar *password);

static inline void add_to_json (gpointer list_elem, gpointer json_array);

static gchar *decrypt_db (const gchar *path, const gchar *password);

static guchar *get_derived_key (const gchar *pwd, HeaderData *header_data);

static void backup_db (const gchar *path);

static void restore_db (const gchar *path);

static void cleanup (GFile *, gpointer, HeaderData *, GError *);


void
load_db (DatabaseData    *db_data,
         GError         **err)
{
    gchar *db_path = g_strconcat (g_get_home_dir (), "/.config/", KF_NAME, NULL);
    if (!g_file_test (db_path, G_FILE_TEST_EXISTS)) {
        g_set_error (err, missing_file_gquark (), MISSING_FILE_CODE, "Missing database file");
        db_data->json_data = NULL;
        return;
    }

    gchar *in_memory_json = decrypt_db (db_path, db_data->key);
    g_free (db_path);

    db_data->json_data = json_from_string (in_memory_json, err);
    gcry_free (in_memory_json);
    if (db_data->json_data == NULL) {
        return;
    }

    JsonArray *ja = json_node_get_array (db_data->json_data);
    for (guint i = 0; i < json_array_get_length (ja); i++) {
        guint hash = json_object_hash (json_array_get_object_element (ja, i));
        db_data->objects_hash = g_slist_append (db_data->objects_hash, GINT_TO_POINTER (hash));
    }
}


void
reload_db (DatabaseData  *db_data,
           GError       **err)
{
    json_node_unref (db_data->json_data);
    load_db (db_data, err);
}


void
update_db (DatabaseData *data)
{
    gboolean first_run = FALSE;
    if (data->json_data == NULL) {
        first_run = TRUE;
    }
    JsonArray *ja;
    gchar *db_path = g_strconcat (g_get_home_dir (), "/.config/", KF_NAME, NULL);
    if (first_run) {
        // this is the first run, array must be created. No need to backup an empty file
        ja = json_array_new ();
    } else {
        backup_db (db_path);
        ja = json_node_get_array (data->json_data);
    }
    g_slist_foreach (data->data_to_add, add_to_json, ja);
    gchar *plain_data = json_to_string (data->json_data, FALSE);
    if (encrypt_db (plain_data, data->key) != NULL) {
        g_printerr ("Couldn't update the database, restoring the original copy...\n");
        if (!first_run)
            restore_db (db_path);
    }
    g_free (plain_data);
    g_free (db_path);
}


gint
check_duplicate (gconstpointer data,
                 gconstpointer user_data)
{
    guint list_elem = *(guint *) data;
    if (list_elem == GPOINTER_TO_UINT (user_data)) {
        return 0;
    }
    return -1;
}


static inline void
add_to_json (gpointer list_elem,
             gpointer json_array)
{

    json_array_add_element (list_elem, json_array);
}


static gpointer
encrypt_db (const gchar *in_memory_json,
            const gchar *password)
{
    GError *err = NULL;
    gcry_cipher_hd_t hd;
    HeaderData *header_data = g_new0 (HeaderData, 1);

    gcry_create_nonce (header_data->iv, IV_SIZE);
    gcry_create_nonce (header_data->salt, KDF_SALT_SIZE);

    gchar *db_path = g_strconcat (g_get_home_dir (), "/.config/", KF_NAME, NULL);
    GFile *out_file = g_file_new_for_path (db_path);
    GFileOutputStream *out_stream = g_file_replace (out_file, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION, NULL, &err);
    if (err != NULL) {
        g_printerr ("%s\n", err->message);
        cleanup (out_file, NULL, header_data, err);
        return GENERIC_ERROR;
    }
    if (g_output_stream_write (G_OUTPUT_STREAM (out_stream), header_data, sizeof (HeaderData), NULL, &err) == -1) {
        g_printerr ("%s\n", err->message);
        cleanup (out_file, out_stream, header_data, err);
        return GENERIC_ERROR;
    }

    guchar *derived_key = get_derived_key (password, header_data);
    if (derived_key == SECURE_MEMORY_ALLOC_ERR || derived_key == KEY_DERIV_ERR) {
        cleanup (out_file, out_stream, header_data, err);
        g_free (header_data);
        return (gpointer) derived_key;
    }

    gsize input_data_len = strlen (in_memory_json) + 1;
    guchar *enc_buffer = g_malloc0 (input_data_len);

    gcry_cipher_open (&hd, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_GCM, 0);
    gcry_cipher_setkey (hd, derived_key, gcry_cipher_get_algo_keylen (GCRY_CIPHER_AES256));
    gcry_cipher_setiv (hd, header_data->iv, IV_SIZE);
    gcry_cipher_authenticate (hd, header_data, sizeof (HeaderData));
    gcry_cipher_encrypt (hd, enc_buffer, input_data_len, in_memory_json, input_data_len);

    guchar tag[TAG_SIZE];
    gcry_cipher_gettag (hd, tag, TAG_SIZE); //append tag to outfile

    if (g_output_stream_write (G_OUTPUT_STREAM (out_stream), enc_buffer, input_data_len, NULL, &err) == -1) {
        cleanup (out_file, out_stream, header_data, err);
        gcry_cipher_close (hd);
        g_free (enc_buffer);
        gcry_free (derived_key);
        return GENERIC_ERROR;
    }
    if (g_output_stream_write (G_OUTPUT_STREAM (out_stream), tag, TAG_SIZE, NULL, &err) == -1) {
        cleanup (out_file, out_stream, header_data, err);
        gcry_cipher_close (hd);
        g_free (enc_buffer);
        gcry_free (derived_key);
        return GENERIC_ERROR;
    }
    g_object_unref (out_file);
    g_object_unref (out_stream);

    gcry_cipher_close (hd);

    g_free (enc_buffer);
    gcry_free (derived_key);

    return NULL;
}


static gchar *
decrypt_db (const gchar *path,
            const gchar *password)
{
    GError *err = NULL;
    gcry_cipher_hd_t hd;
    HeaderData *header_data = g_new0 (HeaderData, 1);

    goffset input_file_size = get_file_size (path);

    GFile *in_file = g_file_new_for_path (path);
    GFileInputStream *in_stream = g_file_read (in_file, NULL, &err);
    if (err != NULL) {
        g_printerr ("%s\n", err->message);
        cleanup (in_file, NULL, header_data, err);
        return GENERIC_ERROR;
    }
    if (g_input_stream_read (G_INPUT_STREAM (in_stream), header_data, sizeof (HeaderData), NULL, &err) == -1) {
        g_printerr ("%s\n", err->message);
        cleanup (in_file, in_stream, header_data, err);
        return GENERIC_ERROR;
    }

    guchar tag[TAG_SIZE];
    if (!g_seekable_seek (G_SEEKABLE (in_stream), input_file_size - TAG_SIZE, G_SEEK_SET, NULL, &err)) {
        g_printerr ("%s\n", err->message);
        cleanup (in_file, in_stream, header_data, err);
        return GENERIC_ERROR;
    }
    if (g_input_stream_read (G_INPUT_STREAM (in_stream), tag, TAG_SIZE, NULL, &err) == -1) {
        g_printerr ("%s\n", err->message);
        cleanup (in_file, in_stream, header_data, err);
        return GENERIC_ERROR;
    }

    gsize enc_buf_size = input_file_size - sizeof (HeaderData) - TAG_SIZE;
    guchar *enc_buf = g_malloc0 (enc_buf_size);

    if (!g_seekable_seek (G_SEEKABLE (in_stream), sizeof (HeaderData), G_SEEK_SET, NULL, &err)) {
        g_printerr ("%s\n", err->message);
        cleanup (in_file, in_stream, header_data, err);
        g_free (enc_buf);
        return GENERIC_ERROR;
    }
    if (g_input_stream_read (G_INPUT_STREAM (in_stream), enc_buf, enc_buf_size, NULL, &err) == -1) {
        g_printerr ("%s\n", err->message);
        cleanup (in_file, in_stream, header_data, err);
        g_free (enc_buf);
        return GENERIC_ERROR;
    }
    g_object_unref (in_stream);
    g_object_unref (in_file);

    guchar *derived_key = get_derived_key (password, header_data);
    if (derived_key == SECURE_MEMORY_ALLOC_ERR || derived_key == KEY_DERIV_ERR) {
        g_free (header_data);
        g_free (enc_buf);
        return (gpointer) derived_key;
    }

    gcry_cipher_open (&hd, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_GCM, 0);
    gcry_cipher_setkey (hd, derived_key, gcry_cipher_get_algo_keylen (GCRY_CIPHER_AES256));
    gcry_cipher_setiv (hd, header_data->iv, IV_SIZE);
    gcry_cipher_authenticate (hd, header_data, sizeof (HeaderData));

    gchar *dec_buf = gcry_calloc_secure (enc_buf_size, 1);
    gcry_cipher_decrypt (hd, dec_buf, enc_buf_size, enc_buf, enc_buf_size);
    if (gcry_err_code (gcry_cipher_checktag (hd, tag, TAG_SIZE)) == GPG_ERR_CHECKSUM) {
        gcry_cipher_close (hd);
        gcry_free (derived_key);
        g_free (header_data);
        g_free (enc_buf);
        return FILE_CORRUPTED;
    }

    gcry_cipher_close (hd);
    gcry_free (derived_key);
    g_free (header_data);
    g_free (enc_buf);

    return dec_buf;
}


static guchar *
get_derived_key (const gchar    *pwd,
                 HeaderData     *header_data)
{
    gsize key_len = gcry_cipher_get_algo_keylen (GCRY_CIPHER_AES256);
    gsize pwd_len = strlen (pwd) + 1;

    guchar *derived_key = gcry_malloc_secure (key_len);
    if (derived_key == NULL) {
        g_printerr ("Couldn't allocate secure memory\n");
        return SECURE_MEMORY_ALLOC_ERR;
    }

    int ret = gcry_kdf_derive (pwd, pwd_len, GCRY_KDF_PBKDF2, GCRY_MD_SHA512, header_data->salt, KDF_SALT_SIZE, KDF_ITERATIONS, key_len, derived_key);
    if (ret != 0) {
        gcry_free (derived_key);
        g_printerr ("Error during key derivation\n");
        return KEY_DERIV_ERR;
    }
    return derived_key;
}


static void
backup_db (const gchar *path)
{
    GError *err = NULL;
    GFile *src = g_file_new_for_path (path);
    gchar *dst_path = g_strconcat (path, ".bak", NULL);
    GFile *dst = g_file_new_for_path (dst_path);
    g_free (dst_path);
    if (!g_file_copy (src, dst, G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL, NULL, &err)) {
        g_printerr ("Couldn't create the backup file: %s\n", err->message);
        g_clear_error (&err);
    }
    g_object_unref (src);
    g_object_unref (dst);
}


static void
restore_db (const gchar *path)
{
    GError *err = NULL;
    gchar *src_path = g_strconcat (path, ".bak", NULL);
    GFile *src = g_file_new_for_path (src_path);
    GFile *dst = g_file_new_for_path (path);
    g_free (src_path);
    if (!g_file_copy (src, dst, G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL, NULL, &err)) {
        g_printerr ("Couldn't restore the backup file: %s\n", err->message);
        g_clear_error (&err);
    }
    g_object_unref (src);
    g_object_unref (dst);
}


static void
cleanup (GFile      *in_file,
         gpointer    in_stream,
         HeaderData *header_data,
         GError     *err)
{
    g_object_unref (in_file);
    if (in_stream != NULL)
        g_object_unref (in_stream);
    g_free (header_data);
    g_clear_error (&err);
}