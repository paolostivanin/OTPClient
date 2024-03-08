#include <glib.h>
#include <gio/gio.h>
#include <gcrypt.h>
#include "gquarks.h"
#include "common.h"

static GSList *get_otps_from_encrypted_backup (const gchar       *path,
                                               const gchar       *password,
                                               gint32             max_file_size,
                                               GFile             *in_file,
                                               GFileInputStream  *in_stream,
                                               GError           **err);

static GSList *get_otps_from_plain_backup     (const gchar       *path,
                                               GError           **err);

static GSList *parse_authpro_json_data        (const gchar       *data,
                                               GError           **err);


GSList *
get_authpro_data (const gchar  *path,
                  const gchar  *password,
                  gint32        max_file_size,
                  GError      **err)
{
    GFile *in_file = g_file_new_for_path(path);
    GFileInputStream *in_stream = g_file_read(in_file, NULL, err);
    if (*err != NULL) {
        g_object_unref(in_file);
        return NULL;
    }

    return (password != NULL) ? get_otps_from_encrypted_backup (path, password, max_file_size, in_file, in_stream, err) : get_otps_from_plain_backup (path, err);
}


gchar *
export_authpro (const gchar *export_path,
                const gchar *password,
                json_t      *json_db_data)
{
    GError *err = NULL;
    json_t *root = json_object ();
    json_t *auth_array = json_array ();
    json_object_set (root, "Authenticators", auth_array);
    json_object_set (root, "Categories", json_array());
    json_object_set (root, "AuthenticatorCategories", json_array());
    json_object_set (root, "CustomIcons", json_array());

    json_t *db_obj, *export_obj;
    gsize index;
    gboolean is_steam = FALSE;
    json_array_foreach (json_db_data, index, db_obj) {
        export_obj = json_object ();
        const gchar *issuer = json_string_value (json_object_get (db_obj, "issuer"));
        if (issuer != NULL) {
            if (g_ascii_strcasecmp (issuer, "steam") == 0) {
                json_object_set (export_obj, "Issuer", json_string ("Steam"));
                is_steam = TRUE;
            } else {
                json_object_set(export_obj, "Issuer", json_object_get (db_obj, "issuer"));
            }
        }
        const gchar *label = json_string_value (json_object_get (db_obj, "issuer"));
        if (label != NULL) {
            json_object_set (export_obj, "Username", json_object_get (db_obj, "label"));
        }
        json_object_set (export_obj, "Secret", json_object_get (db_obj, "secret"));
        json_object_set (export_obj, "Digits", json_object_get (db_obj, "digits"));
        json_object_set (export_obj, "Ranking", json_integer (0));
        json_object_set (export_obj, "Icon", json_null());
        json_object_set (export_obj, "Pin", json_null());
        if (g_ascii_strcasecmp (json_string_value (json_object_get (db_obj, "algo")), "SHA1") == 0) {
            json_object_set (export_obj, "Algorithm", json_integer (0));
        } else if (g_ascii_strcasecmp (json_string_value (json_object_get (db_obj, "algo")), "SHA256") == 0) {
            json_object_set (export_obj, "Algorithm", json_integer (1));
        } else if (g_ascii_strcasecmp (json_string_value (json_object_get (db_obj, "algo")), "SHA512") == 0) {
            json_object_set (export_obj, "Algorithm", json_integer (2));
        }
        if (g_ascii_strcasecmp (json_string_value (json_object_get (db_obj, "type")), "TOTP") == 0) {
            json_object_set (export_obj, "Period", json_object_get (db_obj, "period"));
            json_object_set (export_obj, "Counter", json_integer (0));
            json_object_set (export_obj, "Type", is_steam ? json_integer (4) : json_integer (2));
        } else {
            json_object_set (export_obj, "Counter", json_object_get (db_obj, "counter"));
            json_object_set (export_obj, "Period", json_integer (0));
            json_object_set (export_obj, "Type", json_integer (1));
        }
        json_array_append (auth_array, export_obj);
    }

    gchar *json_data = json_dumps (root, JSON_COMPACT);
    if (json_data == NULL) {
        g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "Couldn't dump json data");
        goto end;
    }
    gsize json_data_size = strlen (json_data);

    GFile *out_gfile = g_file_new_for_path (export_path);
    GFileOutputStream *out_stream = g_file_replace (out_gfile, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION | G_FILE_CREATE_PRIVATE, NULL, &err);
    if (password != NULL) {
        // encrypt the content and write the encrypted file to disk
        const gchar *header = "AUTHENTICATORPRO";
        guchar *salt = g_malloc0 (AUTHPRO_SALT_TAG);
        gcry_create_nonce (salt, AUTHPRO_SALT_TAG);
        guchar *iv = g_malloc0 (AUTHPRO_IV);
        gcry_create_nonce (iv, AUTHPRO_SALT_TAG);
        guchar *derived_key = get_authpro_derived_key (password, salt);
        if (derived_key == NULL) {
            g_free (salt);
            g_free (iv);
            g_set_error (&err, key_deriv_gquark (), KEY_DERIVATION_ERRCODE, "Error while deriving the key.");
            goto end;
        }
        gcry_cipher_hd_t hd = open_cipher_and_set_data (derived_key, iv, AUTHPRO_IV);
        if (hd == NULL) {
            gcry_free (derived_key);
            g_free (salt);
            g_free (iv);
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "Error while opening the cipher.");
            goto end;
        }
        gchar *enc_buf = gcry_calloc_secure (json_data_size, 1);
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
        guchar tag[AUTHPRO_SALT_TAG];
        gcry_cipher_gettag (hd, tag, AUTHPRO_SALT_TAG);
        gcry_cipher_close (hd);

        if (g_output_stream_write (G_OUTPUT_STREAM(out_stream), header, 16, NULL, &err) == -1) {
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "Couldn't write header to file.");
            goto enc_end;
        }
        if (g_output_stream_write (G_OUTPUT_STREAM(out_stream), salt, AUTHPRO_SALT_TAG, NULL, &err) == -1) {
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "Couldn't write salt to file.");
            goto enc_end;
        }
        if (g_output_stream_write (G_OUTPUT_STREAM(out_stream), iv, AUTHPRO_IV, NULL, &err) == -1) {
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "Couldn't write iv to file.");
            goto enc_end;
        }
        if (g_output_stream_write (G_OUTPUT_STREAM(out_stream), enc_buf, json_data_size, NULL, &err) == -1) {
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "Couldn't write payload to file.");
            goto enc_end;
        }
        if (g_output_stream_write (G_OUTPUT_STREAM(out_stream), tag, AUTHPRO_SALT_TAG, NULL, &err) == -1) {
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "Couldn't write tag to file");
            goto enc_end;
        }
        enc_end:
        gcry_free (derived_key);
        gcry_free (enc_buf);
        g_free (iv);
        g_free (salt);
    } else {
        // write the plain json to disk
        if (g_output_stream_write (G_OUTPUT_STREAM(out_stream), json_data, json_data_size, NULL, &err) == -1) {
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "couldn't dump json data to file");
        }
    }
    g_object_unref (out_stream);
    g_object_unref (out_gfile);

    end:
    gcry_free (json_data);
    json_decref (auth_array);
    json_decref (root);

    return (err != NULL ? g_strdup (err->message) : NULL);
}


static GSList *
get_otps_from_encrypted_backup (const gchar       *path,
                                const gchar       *password,
                                gint32             max_file_size,
                                GFile             *in_file,
                                GFileInputStream  *in_stream,
                                GError           **err)
{
    guchar header[16];
    if (g_input_stream_read (G_INPUT_STREAM (in_stream), header, 16, NULL, err) == -1) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    }

    gchar *decrypted_json = get_data_from_encrypted_backup (path, password, max_file_size, AUTHPRO, 0, in_file, in_stream, err);
    if (decrypted_json == NULL) {
        return NULL;
    }

    GSList *otps = parse_authpro_json_data (decrypted_json, err);
    gcry_free (decrypted_json);

    return otps;
}


static GSList *
get_otps_from_plain_backup (const gchar  *path,
                            GError      **err)
{
    json_error_t j_err;
    json_t *json = json_load_file (path, 0, &j_err);
    if (!json) {
        g_printerr ("Error loading json: %s\n", j_err.text);
        return NULL;
    }

    gchar *dumped_json = json_dumps (json, 0);
    GSList *otps = parse_authpro_json_data (dumped_json, err);
    gcry_free (dumped_json);

    return otps;
}


static GSList *
parse_authpro_json_data (const gchar *data,
                         GError     **err)
{
    json_error_t jerr;
    json_t *root = json_loads (data, JSON_DISABLE_EOF_CHECK, &jerr);
    if (root == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "%s", jerr.text);
        return NULL;
    }

    json_t *array = json_object_get (root, "Authenticators");
    if (array == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "%s", jerr.text);
        json_decref (root);
        return NULL;
    }

    GSList *otps = NULL;
    for (guint i = 0; i < json_array_size (array); i++) {
        json_t *obj = json_array_get (array, i);

        otp_t *otp = g_new0 (otp_t, 1);
        otp->issuer = g_strdup (json_string_value (json_object_get (obj, "Issuer")));
        otp->account_name = g_strdup (json_string_value (json_object_get (obj, "Username")));
        otp->secret = secure_strdup (json_string_value (json_object_get (obj, "Secret")));
        otp->digits = (guint32)json_integer_value (json_object_get(obj, "Digits"));
        otp->counter = json_integer_value (json_object_get (obj, "Counter"));
        otp->period = (guint32)json_integer_value (json_object_get (obj, "Period"));

        gboolean skip = FALSE;
        guint32 algo = (guint32)json_integer_value (json_object_get(obj, "Algorithm"));
        switch (algo) {
            case 0:
                otp->algo = g_strdup ("SHA1");
                break;
            case 1:
                otp->algo = g_strdup ("SHA256");
                break;
            case 2:
                otp->algo = g_strdup ("SHA512");
                break;
            default:
                g_printerr ("Skipping token due to unsupported algo: %d\n", algo);
                skip = TRUE;
                break;
        }

        guint32 type = (guint32)json_integer_value (json_object_get(obj, "Type"));
        switch (type) {
            case 1:
                otp->type = g_strdup ("HOTP");
                break;
            case 2:
                otp->type = g_strdup ("TOTP");
                break;
            case 4:
                otp->type = g_strdup ("TOTP");
                g_free (otp->issuer);
                otp->issuer = g_strdup ("Steam");
                break;
            default:
                g_printerr ("Skipping token due to unsupported type: %d (3=Mobile-OTP, 5=Yandex)\n", type);
                skip = TRUE;
                break;
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

    json_decref (root);

    return otps;
}
