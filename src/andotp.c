#include <glib.h>
#include <gio/gio.h>
#include <gcrypt.h>
#include <json-glib/json-glib.h>
#include "file-size.h"
#include "imports.h"
#include "common.h"
#include "gquarks.h"
#include "otpclient.h"

#define ANDOTP_IV_SIZE 12
#define TAG_SIZE 16

static guchar *get_sha256 (const gchar *password);

static GSList *parse_json_data (const gchar *data, GError **err);


GSList *
get_andotp_data (const gchar     *path,
                 const gchar     *password,
                 gint32           max_file_size,
                 GError         **err)
{
    gcry_cipher_hd_t hd;

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
    guchar tag[TAG_SIZE];
    if (!g_seekable_seek (G_SEEKABLE (in_stream), input_file_size - TAG_SIZE, G_SEEK_SET, NULL, err)) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    }
    if (g_input_stream_read (G_INPUT_STREAM (in_stream), tag, TAG_SIZE, NULL, err) == -1) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    }

    gsize enc_buf_size = (gsize) input_file_size - ANDOTP_IV_SIZE - TAG_SIZE;
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

    gcry_cipher_open (&hd, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_GCM, GCRY_CIPHER_SECURE);
    gcry_cipher_setkey (hd, hashed_key, gcry_cipher_get_algo_keylen (GCRY_CIPHER_AES256));
    gcry_cipher_setiv (hd, iv, ANDOTP_IV_SIZE);

    gchar *decrypted_json = gcry_calloc_secure (enc_buf_size, 1);
    gcry_cipher_decrypt (hd, decrypted_json, enc_buf_size, enc_buf, enc_buf_size);
    if (gcry_err_code (gcry_cipher_checktag (hd, tag, TAG_SIZE)) == GPG_ERR_CHECKSUM) {
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
    JsonParser *parser = json_parser_new ();
    if (!json_parser_load_from_data (parser, data, -1, err)) {
        g_object_unref (parser);
        return NULL;
    }

    JsonNode *root = json_parser_get_root (parser);
    g_return_val_if_fail (JSON_NODE_HOLDS_ARRAY (root), NULL);
    JsonArray *arr = json_node_get_array (root);

    GSList *otps = NULL;
    for (guint i = 0; i < json_array_get_length (arr); i++) {
        JsonNode *node = json_array_get_element (arr, i);
        JsonObject *object = json_node_get_object (node);
        if (object == NULL) {
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Expected to find a json object");
            return NULL;
        }

        otp_t *otp = g_new0 (otp_t, 1);
        otp->secret = secure_strdup (json_object_get_string_member (object, "secret"));

        const gchar *label_with_issuer = json_object_get_string_member (object, "label");
        gchar **tokens = g_strsplit (label_with_issuer, "-", -1);
        if (tokens[0] && tokens[1]) {
            otp->issuer = g_strdup (g_strstrip (tokens[0]));
            otp->label = g_strdup (g_strstrip (tokens[1]));
        } else {
            otp->label = g_strdup (g_strstrip (tokens[0]));
        }
        g_strfreev (tokens);

        otp->period = (guint8) json_object_get_int_member (object, "period");
        otp->digits = (guint8) json_object_get_int_member (object, "digits");

        const gchar *type = json_object_get_string_member (object, "type");
        if (g_ascii_strcasecmp (type, "TOTP") == 0) {
            otp->type = g_strdup (type);
            otp->period = 30;
        } else if (g_ascii_strcasecmp (type, "HOTP") == 0) {
            otp->type = g_strdup (type);
            // TODO read this property from the loaded file when andOTP will implement HOTP
            otp->counter = 0;
        } else {
            g_printerr ("otp type is neither TOTP nor HOTP\n");
            gcry_free (otp->secret);
            return NULL;
        }

        const gchar *algo = json_object_get_string_member (object, "algorithm");
        if (g_ascii_strcasecmp (algo, "SHA1") == 0 ||
            g_ascii_strcasecmp (algo, "SHA256") == 0 ||
            g_ascii_strcasecmp (algo, "SHA512") == 0) {
                otp->algo = g_ascii_strup (algo, -1);
        } else {
            g_printerr ("algo not supported (must be either one of: sha1, sha256 or sha512\n");
            gcry_free (otp->secret);
            return NULL;
        }

        otps = g_slist_append (otps, g_memdup (otp, sizeof (otp_t)));
        g_free (otp);
    }
    g_object_unref (parser);

    return otps;
}