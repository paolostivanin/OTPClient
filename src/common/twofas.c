#include <glib.h>
#include <gio/gio.h>
#include <jansson.h>
#include <gcrypt.h>
#include "common.h"
#include "../gquarks.h"
#include "../imports.h"
#include "../parse-uri.h"

#define TWOFAS_KDF_ITERS 10000

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

static json_t   *get_json_root                  (const gchar       *path);

static void      decrypt_data                   (const gchar      **b64_data,
                                                 const gchar       *pwd,
                                                 TwofasData        *twofas_data);

static gchar    *get_encoded_data               (guchar            *enc_buf,
                                                 gsize              enc_buf_len,
                                                 guchar            *salt,
                                                 guchar            *iv);

static GSList   *parse_twofas_json_data         (const gchar       *data,
                                                 GError           **err);


GSList *
get_twofas_data (const gchar  *path,
                 const gchar  *password,
                 GError      **err)
{
    return (password != NULL) ? get_otps_from_encrypted_backup (path, password, err) : get_otps_from_plain_backup (path, err);
}


gchar *
export_twofas (const gchar *export_path,
               const gchar *password,
               json_t      *json_db_data)
{
    // TODO: create the otps json (services => array(secret,
    //                                               order => object(position=0++),
    //                                               icon=null),
    GError *err = NULL;
    json_t *root = json_object ();
    json_t *services_array = json_array ();
    json_object_set (root, "services", services_array);
    json_object_set (root, "groups", json_array());
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
        const gchar *label = json_string_value (json_object_get (db_obj, "label"));
        if (label != NULL) {
            json_object_set (otp_obj, "label", json_string (label));
            json_object_set (otp_obj, "account", json_string (label));
        }

        gchar *algo = g_ascii_strup (json_string_value (json_object_get (db_obj, "algo")), -1);
        json_object_set (otp_obj, "algorithm", json_object_get (db_obj, "digits"));
        g_free (algo);

        json_object_set (otp_obj, "digits", json_string (algo));
        json_object_set (otp_obj, "source", json_string ("Link"));
        gchar *otpauth_uri = secure_strdup (get_otpauth_uri (NULL, db_obj));
        json_object_set (otp_obj, "link", json_string (otpauth_uri));
        gcry_free (otpauth_uri);

        if (g_ascii_strcasecmp (json_string_value (json_object_get (db_obj, "type")), "TOTP") == 0) {
            json_object_set (otp_obj, "period", json_object_get (db_obj, "period"));
            json_object_set (otp_obj, "tokenType", json_string ("TOTP"));
        } else {
            json_object_set (otp_obj, "counter", json_object_get (db_obj, "counter"));
            json_object_set (otp_obj, "tokenType", json_string ("HOTP"));
        }

        json_object_set (order_obj, "position", json_integer ((json_int_t)index));
        json_object_set (export_obj, "order", order_obj);
        json_object_set (export_obj, "otp", otp_obj);
        json_object_set (export_obj, "icon", json_null());

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
        guchar *enc_buf = gcry_calloc_secure (json_data_size, 1);
        gpg_error_t gpg_err = gcry_cipher_encrypt (hd, enc_buf, json_data_size, json_data, json_data_size);
        if (gpg_err != GPG_ERR_NO_ERROR) {
            g_printerr ("Failed to encrypt data: %s/%s\n", gcry_strsource (gpg_err), gcry_strerror (gpg_err));
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "Failed to encrypt data.");
            gcry_free (derived_key);
            gcry_free (enc_buf);
            g_free (iv);
            g_free (salt);
            gcry_cipher_close (hd);
            goto end;
        }
        // TWOFAS ignores the tag, so we don't have to append it (sigh!)
        gcry_free (derived_key);
        gcry_cipher_close (hd);

        json_t *enc_root = json_object ();
        json_object_set (enc_root, "services", json_array ());
        json_object_set (enc_root, "groups", json_array());
        json_object_set (enc_root, "schemaVersion", json_integer (4));
        json_object_set (enc_root, "reference", json_null ());
        gchar *encoded_data = get_encoded_data (enc_buf, json_data_size, salt, iv);
        json_object_set (enc_root, "servicesEncrypted", json_string (encoded_data));
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


static json_t *
get_json_root (const gchar *path)
{
    json_error_t jerr;
    json_t *json = json_load_file (path, 0, &jerr);
    if (!json) {
        g_printerr ("Error loading json: %s\n", jerr.text);
        return FALSE;
    }

    gchar *dumped_json = json_dumps (json, 0);
    json_t *root = json_loads (dumped_json, JSON_DISABLE_EOF_CHECK, &jerr);
    gcry_free (dumped_json);

    return root;
}


static void
decrypt_data (const gchar **b64_data,
              const gchar *pwd,
              TwofasData   *twofas_data)
{
    // TWOFAS ignores the tag, so we don't have to check it (sigh!)
    gsize data_out_len, salt_out_len, iv_out_len;
    guchar *enc_data = g_base64_decode (b64_data[0], &data_out_len);
    twofas_data->salt = g_base64_decode (b64_data[1], &salt_out_len);
    twofas_data->iv = g_base64_decode (b64_data[2], &iv_out_len);

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

    twofas_data->json_data = gcry_calloc_secure (data_out_len, 1);
    gpg_error_t gpg_err = gcry_cipher_decrypt (hd, twofas_data->json_data, data_out_len, enc_data, data_out_len);
    if (gpg_err) {
        g_printerr ("Failed to decrypt data: %s/%s\n", gcry_strsource (g_err), gcry_strerror (g_err));
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
            otps = g_slist_append (otps, g_memdup2 (otp, sizeof (otp_t)));
        }

        gcry_free (otp->secret);
        g_free (otp->issuer);
        g_free (otp->account_name);
        g_free (otp->algo);
        g_free (otp->type);
        g_free (otp);
    }

    json_decref (array);

    return otps;
}