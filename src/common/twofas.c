#include <glib.h>
#include <gio/gio.h>
#include <jansson.h>
#include <gcrypt.h>
#include "gquarks.h"
#include "common.h"
#include "file-size.h"

#define TWOFAS_KDF_ITERS 10000
#define TWOFAS_SALT      256
#define TWOFAS_IV        12
#define TWOFAS_TAG       16

typedef struct twofas_data_t {
    guchar *salt;
    guchar *iv;
    gchar *json_data;
} TwofasData;

static GSList   *get_otps_from_encrypted_backup (const gchar       *path,
                                                 const gchar       *password,
                                                 GError           **err);

static GSList   *get_otps_from_plain_backup     (const gchar       *path,
                                                 GError           **err);

static gboolean  is_schema_supported            (const gchar       *path);

static void      decrypt_data                   (const gchar      **b64_data,
                                                 const gchar       *pwd,
                                                 TwofasData        *twofas_data);

static gchar    *get_encoded_data               (guchar            *enc_buf,
                                                 gsize              enc_buf_len,
                                                 guchar            *salt,
                                                 guchar            *iv);

static gchar    *get_reference_data             (guchar            *derived_key,
                                                 guchar            *salt);

static GSList   *parse_twofas_json_data         (const gchar       *data,
                                                 GError           **err);


GSList *
get_twofas_data (const gchar  *path,
                 const gchar  *password,
                 gint32        max_file_size,
                 GError      **err)
{
    if (get_file_size (path) > max_file_size) {
        g_set_error (err, file_too_big_gquark (), FILE_TOO_BIG_ERRCODE, FILE_SIZE_SECMEM_MSG);
        return NULL;
    }
    return (password != NULL) ? get_otps_from_encrypted_backup (path, password, err) : get_otps_from_plain_backup (path, err);
}


gchar *
export_twofas (const gchar *export_path,
               const gchar *password,
               json_t      *json_db_data)
{
    GError *err = NULL;
    gint64 epoch_time = g_get_real_time();

    json_t *root = json_object ();
    json_t *services_array = json_array ();
    json_object_set (root, "services", services_array);
    json_object_set (root, "groups", json_array());
    json_object_set (root, "updatedAt", json_integer (epoch_time));
    json_object_set (root, "schemaVersion", json_integer (4));

    json_t *db_obj, *export_obj, *otp_obj, *order_obj;
    gsize index;
    json_array_foreach (json_db_data, index, db_obj) {
        export_obj = json_object ();
        otp_obj = json_object ();
        order_obj = json_object ();
        const gchar *issuer = json_string_value (json_object_get (db_obj, "issuer"));
        if (issuer != NULL) {
            if (g_ascii_strcasecmp (issuer, "steam") == 0) {
                json_object_set (export_obj, "name", json_string ("Steam"));
                json_object_set (otp_obj, "issuer", json_string ("Steam"));
                json_object_set (otp_obj, "tokenType", json_string ("STEAM"));
            } else {
                json_object_set(export_obj, "name", json_string (issuer));
                json_object_set (otp_obj, "issuer", json_string (issuer));
            }
        }
        json_object_set (export_obj, "updatedAt", json_integer (epoch_time));
        json_object_set (export_obj, "secret", json_object_get (db_obj, "secret"));
        const gchar *label = json_string_value (json_object_get (db_obj, "label"));
        if (label != NULL) {
            json_object_set (otp_obj, "label", json_string (label));
            json_object_set (otp_obj, "account", json_string (label));
        }

        gchar *algo = g_ascii_strup (json_string_value (json_object_get (db_obj, "algo")), -1);
        json_object_set (otp_obj, "algorithm", json_string (algo));
        g_free (algo);

        json_object_set (otp_obj, "digits", json_object_get (db_obj, "digits"));
        json_object_set (otp_obj, "source", json_string ("Manual"));

        if (g_ascii_strcasecmp (json_string_value (json_object_get (db_obj, "type")), "TOTP") == 0) {
            json_object_set (otp_obj, "period", json_object_get (db_obj, "period"));
            json_object_set (otp_obj, "tokenType", json_string ("TOTP"));
        } else {
            json_object_set (otp_obj, "counter", json_object_get (db_obj, "counter"));
            json_object_set (otp_obj, "tokenType", json_string ("HOTP"));
        }

        json_object_set (order_obj, "position", json_integer ((json_int_t)index));
        json_object_set (export_obj, "otp", otp_obj);
        json_object_set (export_obj, "order", order_obj);

        json_array_append (services_array, export_obj);
    }

    gchar *json_data = json_dumps ((password == NULL) ? root : services_array, JSON_COMPACT);
    if (json_data == NULL) {
        g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "Couldn't dump json data");
        goto end;
    }
    gsize json_data_size = strlen (json_data);

    GFile *out_gfile = g_file_new_for_path (export_path);
    GFileOutputStream *out_stream = g_file_replace (out_gfile, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION | G_FILE_CREATE_PRIVATE, NULL, &err);
    if (password != NULL) {
        guchar *salt = g_malloc0 (TWOFAS_SALT);
        gcry_create_nonce (salt, TWOFAS_SALT);
        guchar *iv = g_malloc0 (TWOFAS_IV);
        gcry_create_nonce (iv, TWOFAS_IV);
        guchar *derived_key = gcry_malloc_secure (32);
        gpg_error_t g_err = gcry_kdf_derive (password, (gsize)g_utf8_strlen (password, -1), GCRY_KDF_PBKDF2, GCRY_MD_SHA256,
                                             salt, TWOFAS_SALT, TWOFAS_KDF_ITERS, 32, derived_key);
        if (g_err != GPG_ERR_NO_ERROR) {
            g_printerr ("Failed to derive key: %s/%s\n", gcry_strsource (g_err), gcry_strerror (g_err));
            g_set_error (&err, key_deriv_gquark (), KEY_DERIVATION_ERRCODE, "Error while deriving the key.");
            gcry_free (derived_key);
            g_free (salt);
            g_free (iv);
            goto end;
        }
        gcry_cipher_hd_t hd = open_cipher_and_set_data (derived_key, iv, TWOFAS_IV);
        if (hd == NULL) {
            gcry_free (derived_key);
            g_free (salt);
            g_free (iv);
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "Error while opening the cipher.");
            goto end;
        }
        guchar *enc_buf = g_malloc0 (json_data_size);
        gpg_error_t gpg_err = gcry_cipher_encrypt (hd, enc_buf, json_data_size, json_data, json_data_size);
        if (gpg_err != GPG_ERR_NO_ERROR) {
            g_printerr ("Failed to encrypt data: %s/%s\n", gcry_strsource (gpg_err), gcry_strerror (gpg_err));
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "Failed to encrypt data.");
            gcry_free (derived_key);
            g_free (enc_buf);
            g_free (iv);
            g_free (salt);
            gcry_cipher_close (hd);
            goto end;
        }
        guchar tag[TWOFAS_TAG];
        gcry_cipher_gettag (hd, tag, TWOFAS_TAG);
        gcry_cipher_close (hd);

        guchar *enc_data_with_tag = g_malloc0 (json_data_size + TWOFAS_TAG);
        memcpy (enc_data_with_tag, enc_buf, json_data_size);
        memcpy (enc_data_with_tag+json_data_size, tag, TWOFAS_TAG);
        g_free (enc_buf);

        json_t *enc_root = json_object ();
        json_object_set (enc_root, "services", json_array ());
        json_object_set (enc_root, "groups", json_array());
        json_object_set (enc_root, "schemaVersion", json_integer (4));
        gchar *encoded_data = get_encoded_data (enc_data_with_tag, json_data_size + TWOFAS_TAG, salt, iv);
        json_object_set (enc_root, "servicesEncrypted", json_string (encoded_data));
        gchar *encoded_ref_data = get_reference_data (derived_key, salt);
        if (encoded_ref_data == NULL) {
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "Couldn't encrypt the reference data.");
            goto enc_end;
        }
        json_object_set (enc_root, "reference", json_string (encoded_ref_data));
        gchar *json_enc_data = json_dumps (enc_root, JSON_COMPACT);
        if (json_enc_data == NULL) {
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "Couldn't dump json data");
            goto enc_end;
        }
        if (g_output_stream_write (G_OUTPUT_STREAM(out_stream), json_enc_data, strlen (json_enc_data), NULL, &err) == -1) {
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "Couldn't write the json data to file");
        }
        gcry_free (json_enc_data);

        enc_end:
        g_free (enc_data_with_tag);
        gcry_free (derived_key);
        g_free (iv);
        g_free (salt);
        g_free (encoded_data);
        json_decref (enc_root);
    } else {
        // write the plain json to disk
        if (g_output_stream_write (G_OUTPUT_STREAM(out_stream), json_data, json_data_size, NULL, &err) == -1) {
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "Couldn't write the json data to file");
        }
    }
    g_object_unref (out_stream);
    g_object_unref (out_gfile);

    end:
    gcry_free (json_data);
    json_decref (services_array);
    json_decref (root);

    return (err != NULL ? g_strdup (err->message) : NULL);
}


static GSList *
get_otps_from_encrypted_backup (const gchar       *path,
                                const gchar       *password,
                                GError           **err)
{
    if (!is_schema_supported (path)) {
        return NULL;
    }

    TwofasData *twofas_data = g_new0 (TwofasData, 1);
    GSList *otps = NULL;

    json_t *root = get_json_root (path);
    gchar **b64_encoded_data = g_strsplit (json_string_value (json_object_get (root, "servicesEncrypted")), ":", 3);
    decrypt_data ((const gchar **)b64_encoded_data, password, twofas_data);
    if (twofas_data->json_data != NULL) {
        otps = parse_twofas_json_data (twofas_data->json_data, err);
        gcry_free (twofas_data->json_data);
    }
    g_strfreev (b64_encoded_data);
    g_free (twofas_data->salt);
    g_free (twofas_data->iv);
    g_free (twofas_data);
    json_decref (root);

    return otps;
}


static GSList *
get_otps_from_plain_backup (const gchar  *path,
                            GError      **err)
{
    if (!is_schema_supported (path)) {
        return NULL;
    }

    json_error_t j_err;
    json_t *json = json_load_file (path, 0, &j_err);
    if (!json) {
        g_printerr ("Error loading json: %s\n", j_err.text);
        return NULL;
    }

    gchar *dumped_json = json_dumps (json_object_get (json, "services"), 0);
    GSList *otps = parse_twofas_json_data (dumped_json, err);
    gcry_free (dumped_json);

    return otps;
}


static gboolean
is_schema_supported (const gchar *path)
{
    json_t *root = get_json_root (path);
    gint32 schema_version = (gint32)json_integer_value (json_object_get (root, "schemaVersion"));
    if (schema_version != 4) {
        g_printerr ("Unsupported schema version: %d\n", schema_version);
        json_decref (root);
        return FALSE;
    }
    json_decref (root);
    return TRUE;
}


static void
decrypt_data (const gchar **b64_data,
              const gchar *pwd,
              TwofasData   *twofas_data)
{
    gsize enc_data_with_tag_size, salt_out_len, iv_out_len;
    guchar *enc_data_with_tag = g_base64_decode (b64_data[0], &enc_data_with_tag_size);
    twofas_data->salt = g_base64_decode (b64_data[1], &salt_out_len);
    twofas_data->iv = g_base64_decode (b64_data[2], &iv_out_len);

    guchar tag[TWOFAS_TAG];
    gsize enc_buf_size = enc_data_with_tag_size - TWOFAS_TAG;
    guchar *enc_data = g_malloc0 (enc_buf_size);
    memcpy (enc_data, enc_data_with_tag, enc_buf_size);
    memcpy (tag, enc_data_with_tag+enc_buf_size, TWOFAS_TAG);
    g_free (enc_data_with_tag);

    guchar *derived_key = gcry_malloc_secure (32);
    gpg_error_t g_err = gcry_kdf_derive (pwd, (gsize)g_utf8_strlen (pwd, -1), GCRY_KDF_PBKDF2, GCRY_MD_SHA256,
                                         twofas_data->salt, salt_out_len, TWOFAS_KDF_ITERS, 32, derived_key);
    if (g_err != GPG_ERR_NO_ERROR) {
        g_printerr ("Failed to derive key: %s/%s\n", gcry_strsource (g_err), gcry_strerror (g_err));
        gcry_free (derived_key);
        g_free (enc_data);
        return;
    }

    gcry_cipher_hd_t hd = open_cipher_and_set_data (derived_key, twofas_data->iv, iv_out_len);
    if (hd == NULL) {
        gcry_free (derived_key);
        g_free (enc_data);
        return;
    }

    twofas_data->json_data = gcry_calloc_secure (enc_buf_size, 1);
    gpg_error_t gpg_err = gcry_cipher_decrypt (hd, twofas_data->json_data, enc_buf_size, enc_data, enc_buf_size);
    if (gpg_err) {
        g_printerr ("Failed to decrypt data: %s/%s\n", gcry_strsource (g_err), gcry_strerror (g_err));
        gcry_free (derived_key);
        g_free (enc_data);
        gcry_cipher_close (hd);
        return;
    }

    gpg_err = gcry_cipher_checktag (hd, tag, TWOFAS_TAG);
    if (gpg_err) {
        g_printerr ("Failed to verify the tag: %s/%s\n", gcry_strsource (g_err), gcry_strerror (g_err));
    }

    gcry_cipher_close (hd);
    gcry_free (derived_key);
    g_free (enc_data);
}


static gchar *
get_encoded_data (guchar *enc_buf,
                  gsize   enc_buf_len,
                  guchar *salt,
                  guchar *iv)
{
    gchar *payload = g_base64_encode (enc_buf, enc_buf_len);
    gchar *encoded_salt = g_base64_encode (salt, TWOFAS_SALT);
    gchar *encoded_iv = g_base64_encode (iv, TWOFAS_IV);
    gchar *encoded_data = g_strconcat (payload, ":", encoded_salt, ":", encoded_iv, NULL);
    g_free (payload);
    g_free (encoded_salt);
    g_free (encoded_iv);

    return encoded_data;
}


static gchar *
get_reference_data (guchar *derived_key,
                    guchar *salt)
{
    // This is taken from https://github.com/twofas/2fas-android/blob/main/data/services/src/main/java/com/twofasapp/data/services/domain/BackupContent.kt
    const gchar *reference = "tRViSsLKzd86Hprh4ceC2OP7xazn4rrt4xhfEUbOjxLX8Rc3mkISXE0lWbmnWfggogbBJhtYgpK6fMl1D6mtsy92R3HkdGfwuXbzLebqVFJsR7IZ2w58t938iymwG4824igYy1wi6n2WDpO1Q1P69zwJGs2F5a1qP4MyIiDSD7NCV2OvidXQCBnDlGfmz0f1BQySRkkt4ryiJeCjD2o4QsveJ9uDBUn8ELyOrESv5R5DMDkD4iAF8TXU7KyoJujd";

    // 2FAS requires a new IV for this reference data
    guchar *iv = g_malloc0 (TWOFAS_IV);
    gcry_create_nonce (iv, TWOFAS_IV);

    gcry_cipher_hd_t hd = open_cipher_and_set_data (derived_key, iv, TWOFAS_IV);
    if (hd == NULL) {
        g_printerr ("Failed to open the cipher to encrypt the reference data.\n");
        return NULL;
    }
    gsize buf_size = strlen (reference);
    guchar *enc_ref_buf = g_malloc0 (buf_size);
    gpg_error_t gpg_err = gcry_cipher_encrypt (hd, enc_ref_buf, buf_size, reference, buf_size);
    if (gpg_err != GPG_ERR_NO_ERROR) {
        g_printerr ("Failed to encrypt the data: %s/%s\n", gcry_strsource (gpg_err), gcry_strerror (gpg_err));
        g_free (enc_ref_buf);
        gcry_cipher_close (hd);
        return NULL;
    }
    guchar tag[TWOFAS_TAG];
    gcry_cipher_gettag (hd, tag, TWOFAS_TAG);
    gcry_cipher_close (hd);

    gsize enc_data_with_tag_size = buf_size + TWOFAS_TAG;
    guchar *enc_data_with_tag = g_malloc0 (enc_data_with_tag_size);
    memcpy (enc_data_with_tag, enc_ref_buf, buf_size);
    memcpy (enc_data_with_tag+buf_size, tag, TWOFAS_TAG);
    g_free (enc_ref_buf);

    gchar *encoded_data = get_encoded_data (enc_data_with_tag, enc_data_with_tag_size, salt, iv);
    g_free (enc_data_with_tag);
    g_free (iv);

    return encoded_data;
}


static GSList *
parse_twofas_json_data (const gchar *data,
                        GError     **err)
{
    json_error_t jerr;
    json_t *array = json_loads (data, JSON_DISABLE_EOF_CHECK, &jerr);
    if (array == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "%s", jerr.text);
        return NULL;
    }

    GSList *otps = NULL;
    for (guint i = 0; i < json_array_size (array); i++) {
        json_t *obj = json_array_get (array, i);

        otp_t *otp = g_new0 (otp_t, 1);
        otp->secret = secure_strdup (json_string_value (json_object_get (obj, "secret")));

        json_t *otp_obj = json_object_get (obj, "otp");
        otp->issuer = g_strdup (json_string_value (json_object_get (otp_obj, "issuer")));
        otp->account_name = g_strdup (json_string_value (json_object_get (otp_obj, "account")));
        otp->digits = (guint32) json_integer_value (json_object_get (otp_obj, "digits"));

        gboolean skip = FALSE;
        const gchar *type = json_string_value (json_object_get (otp_obj, "tokenType"));
        if (g_ascii_strcasecmp (type, "TOTP") == 0) {
            otp->type = g_strdup ("TOTP");
            otp->period = (guint32)json_integer_value (json_object_get (otp_obj, "period"));
        } else if (g_ascii_strcasecmp (type, "HOTP") == 0) {
            otp->type = g_strdup ("HOTP");
            otp->counter = json_integer_value (json_object_get (otp_obj, "counter"));
        } else if (g_ascii_strcasecmp (type, "Steam") == 0) {
            otp->type = g_strdup ("TOTP");
            otp->period = (guint32)json_integer_value (json_object_get (otp_obj, "period"));
            g_free (otp->issuer);
            otp->issuer = g_strdup ("Steam");
        } else {
            g_printerr ("Skipping token due to unsupported type: %s\n", type);
            skip = TRUE;
        }

        const gchar *algo = json_string_value (json_object_get (otp_obj, "algorithm"));
        if (g_ascii_strcasecmp (algo, "SHA1") == 0 ||
            g_ascii_strcasecmp (algo, "SHA256") == 0 ||
            g_ascii_strcasecmp (algo, "SHA512") == 0) {
            otp->algo = g_utf8_strup (algo, -1);
        } else {
            g_printerr ("Skipping token due to unsupported algo: %s\n", algo);
            skip = TRUE;
        }

        if (!skip) {
            otps = g_slist_append (otps, otp);
        } else {
            gcry_free (otp->secret);
            g_free (otp->issuer);
            g_free (otp->account_name);
            g_free (otp->algo);
            g_free (otp->type);
            g_free (otp);
        }
    }

    json_decref (array);

    return otps;
}
