#include <glib.h>
#include <gio/gio.h>
#include <gcrypt.h>
#include <jansson.h>
#include "file-size.h"
#include "imports.h"
#include "common.h"
#include "gquarks.h"

#define ANDOTP_IV_SIZE  12
#define ANDOTP_TAG_SIZE 16

static guchar *get_sha256 (const gchar *password);

static GSList *parse_json_data (const gchar *data, GError **err);


GSList *
get_andotp_data (const gchar     *path,
                 const gchar     *password,
                 gint32           max_file_size,
                 GError         **err)
{
    GFile *in_file = g_file_new_for_path (path);
    GFileInputStream *in_stream = g_file_read (in_file, NULL, err);
    if (*err != NULL) {
        g_object_unref (in_file);
        return NULL;
    }

    guchar iv[ANDOTP_IV_SIZE];
    if (g_input_stream_read (G_INPUT_STREAM (in_stream), iv, ANDOTP_IV_SIZE, NULL, err) == -1) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    }

    goffset input_file_size = get_file_size (path);
    guchar tag[ANDOTP_TAG_SIZE];
    if (!g_seekable_seek (G_SEEKABLE (in_stream), input_file_size - ANDOTP_TAG_SIZE, G_SEEK_SET, NULL, err)) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    }
    if (g_input_stream_read (G_INPUT_STREAM (in_stream), tag, ANDOTP_TAG_SIZE, NULL, err) == -1) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    }

    gsize enc_buf_size = (gsize) input_file_size - ANDOTP_IV_SIZE - ANDOTP_TAG_SIZE;
    if (enc_buf_size < 1) {
        g_printerr ("A non-encrypted file has been selected\n");
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    } else if (enc_buf_size > max_file_size) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        g_set_error (err, file_too_big_gquark (), FILE_TOO_BIG, "File is too big");
        return NULL;
    }
    guchar *enc_buf = g_malloc0 (enc_buf_size);

    if (!g_seekable_seek (G_SEEKABLE (in_stream), ANDOTP_IV_SIZE, G_SEEK_SET, NULL, err)) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        g_free (enc_buf);
        return NULL;
    }
    if (g_input_stream_read (G_INPUT_STREAM (in_stream), enc_buf, enc_buf_size, NULL, err) == -1) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        g_free (enc_buf);
        return NULL;
    }
    g_object_unref (in_stream);
    g_object_unref (in_file);

    guchar *hashed_key = get_sha256 (password);

    gcry_cipher_hd_t hd;
    gcry_cipher_open (&hd, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_GCM, GCRY_CIPHER_SECURE);
    gcry_cipher_setkey (hd, hashed_key, gcry_cipher_get_algo_keylen (GCRY_CIPHER_AES256));
    gcry_cipher_setiv (hd, iv, ANDOTP_IV_SIZE);

    gchar *decrypted_json = gcry_calloc_secure (enc_buf_size, 1);
    gcry_cipher_decrypt (hd, decrypted_json, enc_buf_size, enc_buf, enc_buf_size);
    if (gcry_err_code (gcry_cipher_checktag (hd, tag, ANDOTP_TAG_SIZE)) == GPG_ERR_CHECKSUM) {
        g_set_error (err, bad_tag_gquark (), BAD_TAG_ERRCODE, "Either the file is corrupted or the password is wrong");
        gcry_cipher_close (hd);
        gcry_free (hashed_key);
        g_free (enc_buf);
        return NULL;
    }

    gcry_cipher_close (hd);
    gcry_free (hashed_key);
    g_free (enc_buf);

    GSList *otps = parse_json_data (decrypted_json, err);
    gcry_free (decrypted_json);

    return otps;
}


gchar *
export_andotp ( const gchar *export_path,
                const gchar *password,
                json_t *json_db_data)
{
    json_t *array = json_array ();
    json_t *db_obj, *export_obj;
    gsize index;
    json_array_foreach (json_db_data, index, db_obj) {
        export_obj = json_object ();

        if (g_ascii_strcasecmp (json_string_value (json_object_get (db_obj, "issuer")), "steam") == 0) {
            json_object_set (export_obj, "type", json_string ("STEAM"));
        } else {
            json_object_set (export_obj, "type", json_object_get (db_obj, "type"));
        }

        gchar *constructed_label;
        const gchar *issuer_from_db = json_string_value (json_object_get (db_obj, "issuer"));
        if (issuer_from_db != NULL && g_utf8_strlen (issuer_from_db, -1) > 0) {
            constructed_label = g_strconcat (json_string_value (json_object_get (db_obj, "issuer")),
                                            ":",
                                            json_string_value (json_object_get (db_obj, "label")),
                                            NULL);
        } else {
            constructed_label = g_strdup (json_string_value (json_object_get (db_obj, "label")));
        }
        json_object_set (export_obj, "label", json_string (constructed_label));
        g_free (constructed_label);
        json_object_set (export_obj, "secret", json_object_get (db_obj, "secret"));
        json_object_set (export_obj, "digits", json_object_get (db_obj, "digits"));
        json_object_set (export_obj, "algorithm", json_object_get (db_obj, "algo"));
        if (g_strcmp0 (json_string_value (json_object_get (db_obj, "type")), "TOTP") == 0) {
            json_object_set (export_obj, "period", json_object_get (db_obj, "period"));
        } else {
            json_object_set (export_obj, "counter", json_object_get (db_obj, "counter"));
        }
        json_array_append (array, export_obj);
    }
    gchar *json_data = json_dumps (array, JSON_COMPACT);
    if (json_data == NULL) {
        json_array_clear (array);
        return g_strdup ("couldn't dump json data");
    }
    gsize json_data_size = g_utf8_strlen (json_data, -1);

    gcry_cipher_hd_t hd;
    gcry_cipher_open (&hd, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_GCM, GCRY_CIPHER_SECURE);
    guchar *hashed_key = get_sha256 (password);
    gcry_cipher_setkey (hd, hashed_key, gcry_cipher_get_algo_keylen (GCRY_CIPHER_AES256));
    guchar *iv = g_malloc0 (ANDOTP_IV_SIZE);
    gcry_create_nonce (iv, ANDOTP_IV_SIZE);
    gcry_cipher_setiv (hd, iv, ANDOTP_IV_SIZE);

    gchar *enc_buf = gcry_calloc_secure (json_data_size, 1);
    gcry_cipher_encrypt (hd, enc_buf, json_data_size, json_data, json_data_size);
    guchar tag[ANDOTP_TAG_SIZE];
    gcry_cipher_gettag (hd, tag, ANDOTP_TAG_SIZE);
    gcry_cipher_close (hd);

    GError *err = NULL;
    GFile *out_gfile = g_file_new_for_path (export_path);
    GFileOutputStream *out_stream = g_file_append_to (out_gfile, G_FILE_CREATE_REPLACE_DESTINATION, NULL, &err);
    if (err != NULL) {
        goto cleanup_before_exiting;
    }
    g_output_stream_write (G_OUTPUT_STREAM (out_stream), iv, ANDOTP_IV_SIZE, NULL, &err);
    if (err != NULL) {
        goto cleanup_before_exiting;
    }
    g_output_stream_write (G_OUTPUT_STREAM (out_stream), enc_buf, json_data_size, NULL, &err);
    if (err != NULL) {
        goto cleanup_before_exiting;
    }
    g_output_stream_write (G_OUTPUT_STREAM (out_stream), tag, ANDOTP_TAG_SIZE, NULL, &err);
    if (err != NULL) {
        goto cleanup_before_exiting;
    }

    cleanup_before_exiting:
    g_free (iv);
    gcry_free (json_data);
    gcry_free (enc_buf);
    json_array_clear (array);
    g_object_unref (out_stream);
    g_object_unref (out_gfile);

    return (err != NULL ? g_strdup (err->message) : NULL);
}


static guchar *
get_sha256 (const gchar *password)
{
    gcry_md_hd_t hd;
    gcry_md_open (&hd, GCRY_MD_SHA256, 0);
    gcry_md_write (hd, password, strlen (password));
    gcry_md_final (hd);

    guchar *key = gcry_calloc_secure (gcry_md_get_algo_dlen (GCRY_MD_SHA256), 1);

    guchar *tmp_hash = gcry_md_read (hd, GCRY_MD_SHA256);
    memcpy (key, tmp_hash, gcry_md_get_algo_dlen (GCRY_MD_SHA256));

    gcry_md_close (hd);

    return key;
}


static GSList *
parse_json_data (const gchar *data,
                 GError     **err)
{
    json_error_t jerr;
    json_t *array = json_loads (data, 0, &jerr);
    if (array == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "%s", jerr.text);
        return NULL;
    }

    GSList *otps = NULL;
    for (guint i = 0; i < json_array_size (array); i++) {
        json_t *obj = json_array_get (array, i);

        otp_t *otp = g_new0 (otp_t, 1);
        otp->secret = secure_strdup (json_string_value (json_object_get (obj, "secret")));

        const gchar *account_with_issuer = json_string_value (json_object_get (obj, "label"));
        gchar **tokens = g_strsplit (account_with_issuer, ":", -1);
        if (tokens[0] && tokens[1]) {
            otp->issuer = g_strdup (g_strstrip (tokens[0]));
            otp->account_name = g_strdup (g_strstrip (tokens[1]));
        } else {
            otp->account_name = g_strdup (g_strstrip (tokens[0]));
        }
        g_strfreev (tokens);

        otp->digits = (guint32) json_integer_value (json_object_get(obj, "digits"));

        const gchar *type = json_string_value (json_object_get (obj, "type"));
        if (g_ascii_strcasecmp (type, "TOTP") == 0) {
            otp->type = g_strdup (type);
            otp->period = (guint32)json_integer_value (json_object_get (obj, "period"));
        } else if (g_ascii_strcasecmp (type, "HOTP") == 0) {
            otp->type = g_strdup (type);
            otp->counter = json_integer_value (json_object_get (obj, "counter"));
        } else if (g_ascii_strcasecmp (type, "Steam") == 0) {
            otp->type = g_strdup ("TOTP");
            otp->period = (guint32)json_integer_value (json_object_get (obj, "period"));
            if (otp->period == 0) {
                // andOTP exported backup for Steam might not contain the period field,
                otp->period = 30;
            }
            g_free (otp->issuer);
            otp->issuer = g_strdup ("Steam");
        } else {
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "otp type is neither TOTP nor HOTP");
            gcry_free (otp->secret);
            g_free (otp);
            json_decref (obj);
            return NULL;
        }

        const gchar *algo = json_string_value (json_object_get (obj, "algorithm"));
        if (g_ascii_strcasecmp (algo, "SHA1") == 0 ||
            g_ascii_strcasecmp (algo, "SHA256") == 0 ||
            g_ascii_strcasecmp (algo, "SHA512") == 0) {
                otp->algo = g_ascii_strup (algo, -1);
        } else {
            g_printerr ("algo not supported (must be either one of: sha1, sha256 or sha512\n");
            gcry_free (otp->secret);
            g_free (otp);
            json_decref (obj);
            return NULL;
        }

        otps = g_slist_append (otps, g_memdup (otp, sizeof (otp_t)));
        g_free (otp);
    }

    json_decref (array);

    return otps;
}