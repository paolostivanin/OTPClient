#include <gtk/gtk.h>
#include <gcrypt.h>
#include "kf-misc.h"
#include "otpclient.h"

static guchar *get_derived_key (const gchar *pwd, HeaderData *header_data);

static gboolean create_kf (const gchar *path);

static void cleanup_enc (guchar *, gchar *, guchar *);

static void cleanup (GFile *, gpointer, HeaderData *, GError *);


gchar *
load_kf (const gchar *plain_key)
{
    GError *err = NULL;

    gchar *kf_path = g_strconcat (g_get_home_dir (), "/.config/", KF_NAME, NULL);
    if (!g_file_test (kf_path, G_FILE_TEST_EXISTS)) {
        create_kf (kf_path);
        encrypt_kf (kf_path, plain_key);
        return FILE_EMPTY; // kf just created so it's empty
    }

    gchar *in_memory_kf = decrypt_kf (kf_path, plain_key);
    g_free (kf_path);

    GKeyFile *kf = NULL;
    g_key_file_load_from_data (kf, in_memory_kf, (gsize)-1, G_KEY_FILE_NONE, &err);
    if (err != NULL) {
        g_printerr ("%s\n", err->message);
        g_clear_error (&err);
        return NULL;
    }

    return in_memory_kf;
}


int
update_kf (GtkWidget *btn, gpointer user_data)
{
    // the file is decrypted when the program boots
    UpdateData *data = (UpdateData *) user_data;
    gchar *kf_path = g_strconcat (g_get_home_dir (), "/.config/", KF_NAME, NULL);
    gboolean btn_is_add;
    if (g_strcmp0 (gtk_widget_get_name (btn), "add_data") == 0) {
        btn_is_add = TRUE;
    } else {
        btn_is_add = FALSE;
    }

    if (g_hash_table_size (data->data_to_add) > 0) {
        GError *err = NULL;
        GKeyFile *kf = g_key_file_new ();
        g_key_file_load_from_data (kf, data->in_memory_kf, (gsize) -1, G_KEY_FILE_NONE, &err);
        if (err != NULL) {
            g_printerr ("Couldn't load key file: %s\n", err->message);
            g_free (kf_path);
            g_clear_error (&err);
            return KF_UPDATE_FAILED;
        }

        GHashTableIter iter;
        gpointer key, value;
        g_hash_table_iter_init (&iter, data->data_to_add);
        while (g_hash_table_iter_next (&iter, &key, &value)) {
            if (btn_is_add) {
                g_key_file_set_string (kf, KF_GROUP, (gchar *)key, (gchar *)value);
            } else {
                g_key_file_remove_key (kf, KF_GROUP, (gchar *)key, NULL);
            }
        }

        // TODO create backup before saving
        if (!g_key_file_save_to_file (kf, kf_path, &err)) {
            g_printerr ("Error while saving file: %s\n", err->message);
            g_clear_error (&err);
        }

        g_key_file_free (kf);
    }

    if (encrypt_kf (kf_path, data->key) != NULL) {
        //  TODO restore backup and report failed update
    }

    g_free (kf_path);

    return KF_UPDATE_OK;
}


static gboolean
create_kf (const gchar *path)
{
    GError *err = NULL;
    gboolean ret = TRUE;

    GKeyFile *kf = g_key_file_new ();

    // workaround to write only the group name to the file
    g_key_file_set_string (kf, KF_GROUP, "test", "test");
    g_key_file_remove_key (kf, KF_GROUP, "test", &err);
    if (err != NULL) {
        g_printerr ("%s\n", err->message);
        ret = FALSE;
    }

    g_key_file_save_to_file (kf, path, &err);
    if (err != NULL) {
        g_printerr ("%s\n", err->message);
        ret = FALSE;
    }

    g_clear_error (&err);
    g_key_file_free (kf);

    return ret;
}


gpointer
encrypt_kf (const gchar *path, const gchar *plain_key)
{
    GError *err = NULL;
    gcry_cipher_hd_t hd;
    HeaderData *header_data = g_new0 (HeaderData, 1);

    gcry_create_nonce (header_data->iv, IV_SIZE);
    gcry_create_nonce (header_data->salt, KDF_SALT_SIZE);

    goffset input_file_size = get_file_size (path);
    if (input_file_size > MAX_FILE_SIZE) {
        g_printerr ("Input file is too big: %ld when the max allowed size is %d", input_file_size, MAX_FILE_SIZE);
        return FILE_TOO_BIG;
    }

    GFile *in_file = g_file_new_for_path (path);
    GFileInputStream *in_stream = g_file_read (in_file, NULL, &err);
    if (err != NULL) {
        g_printerr ("%s\n", err->message);
        cleanup (in_file, NULL, header_data, err);
        return GENERIC_ERROR;
    }

    gchar *buffer = gcry_malloc_secure ((gsize) input_file_size);
    if (g_input_stream_read (G_INPUT_STREAM (in_stream), buffer, (gsize) input_file_size, NULL, &err) == -1) {
        g_printerr ("%s\n", err->message);
        cleanup (in_file, in_stream, header_data, err);
        g_free (buffer);
        return GENERIC_ERROR;
    }
    g_object_unref (in_file);
    g_object_unref (in_stream);

    GFile *out_file = g_file_new_for_path (path);
    GFileOutputStream *out_stream = g_file_replace (out_file, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION, NULL, &err);
    if (err != NULL) {
        g_printerr ("%s\n", err->message);
        cleanup (out_file, NULL, header_data, err);
        g_free (buffer);
        return GENERIC_ERROR;
    }
    if (g_output_stream_write (G_OUTPUT_STREAM (out_stream), header_data, sizeof (HeaderData), NULL, &err) == -1) {
        g_printerr ("%s\n", err->message);
        cleanup (out_file, out_stream, header_data, err);
        g_free (buffer);
        return GENERIC_ERROR;
    }

    guchar *derived_key = get_derived_key (plain_key, header_data);
    if (derived_key == SECURE_MEMORY_ALLOC_ERR || derived_key == KEY_DERIV_ERR) {
        cleanup (out_file, out_stream, header_data, err);
        g_free (header_data);
        return (gpointer) derived_key;
    }

    guchar *enc_buffer = g_malloc0 ((gsize) input_file_size);

    gcry_cipher_open (&hd, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_GCM, 0);
    gcry_cipher_setkey (hd, derived_key, gcry_cipher_get_algo_keylen (GCRY_CIPHER_AES256));
    gcry_cipher_setiv (hd, header_data->iv, IV_SIZE);
    gcry_cipher_authenticate (hd, header_data, sizeof (HeaderData));
    gcry_cipher_encrypt (hd, enc_buffer, (gsize) input_file_size, buffer, (gsize) input_file_size);

    guchar tag[TAG_SIZE];
    gcry_cipher_gettag (hd, tag, TAG_SIZE); //append tag to outfile

    if (g_output_stream_write (G_OUTPUT_STREAM (out_stream), enc_buffer, (gsize) input_file_size, NULL, &err) == -1) {
        cleanup (out_file, out_stream, header_data, err);
        gcry_cipher_close (hd);
        cleanup_enc (derived_key, buffer, enc_buffer);
        return GENERIC_ERROR;
    }
    if (g_output_stream_write (G_OUTPUT_STREAM (out_stream), tag, TAG_SIZE, NULL, &err) == -1) {
        cleanup (out_file, out_stream, header_data, err);
        gcry_cipher_close (hd);
        cleanup_enc (derived_key, buffer, enc_buffer);
        return GENERIC_ERROR;
    }
    g_object_unref (out_file);
    g_object_unref (out_stream);

    gcry_cipher_close (hd);

    cleanup_enc (derived_key, buffer, enc_buffer);

    return NULL;
}


gchar *
decrypt_kf (const gchar *path, const gchar *plain_key)
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

    guchar *derived_key = get_derived_key (plain_key, header_data);
    if (derived_key == SECURE_MEMORY_ALLOC_ERR || derived_key == KEY_DERIV_ERR) {
        g_free (header_data);
        g_free (enc_buf);
        return (gpointer) derived_key;
    }

    gcry_cipher_open (&hd, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_GCM, 0);
    gcry_cipher_setkey (hd, derived_key, gcry_cipher_get_algo_keylen (GCRY_CIPHER_AES256));
    gcry_cipher_setiv (hd, header_data->iv, IV_SIZE);
    gcry_cipher_authenticate (hd, header_data, sizeof (HeaderData));

    gchar *dec_buf = gcry_malloc_secure (enc_buf_size);
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
get_derived_key (const gchar *pwd, HeaderData *header_data)
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
cleanup (GFile *in_file, gpointer in_stream, HeaderData *header_data, GError *err)
{
    g_object_unref (in_file);
    if (in_stream != NULL)
        g_object_unref (in_stream);
    g_free (header_data);
    g_clear_error (&err);
}


static void
cleanup_enc (guchar *key, gchar *buf, guchar *enc_buf)
{
    gcry_free (key);
    gcry_free (buf);
    g_free (enc_buf);
}
