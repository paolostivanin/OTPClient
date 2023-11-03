#include <glib.h>
#include <gcrypt.h>
#include <jansson.h>
#include <time.h>
#include <uuid/uuid.h>
#include "../imports.h"
#include "../gquarks.h"
#include "common.h"


#define NONCE_SIZE  12
#define TAG_SIZE    16
#define SALT_SIZE   32
#define KEY_SIZE    32


static GSList *get_otps_from_plain_backup     (const gchar          *path,
                                               GError              **err);

static GSList *get_otps_from_encrypted_backup (const gchar          *path,
                                               const gchar          *password,
                                               gint32                max_file_size,
                                               GError              **err);

static GSList *parse_json_data                (const gchar          *data,
                                               GError              **err);


GSList *
get_aegis_data (const gchar     *path,
                const gchar     *password,
                gint32           max_file_size,
                gboolean         encrypted,
                GError         **err)
{
    if (g_file_test (path, G_FILE_TEST_IS_SYMLINK | G_FILE_TEST_IS_DIR) ) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Selected file is either a symlink or a directory.");
        return NULL;
    }

    return (encrypted == TRUE) ? get_otps_from_encrypted_backup(path, password, max_file_size, err) : get_otps_from_plain_backup(path, err);
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

    gchar *dumped_json = json_dumps(json_object_get (json, "db"), 0);
    GSList *otps = parse_json_data (dumped_json, err);
    gcry_free (dumped_json);

    return otps;
}


static GSList *
get_otps_from_encrypted_backup (const gchar          *path,
                                const gchar          *password,
                                gint32                max_file_size,
                                GError              **err)
{
    json_error_t j_err;
    json_t *json = json_load_file (path, 0, &j_err);
    if (!json) {
        g_printerr ("Error loading json: %s\n", j_err.text);
        return NULL;
    }

    json_t *arr = json_object_get (json_object_get(json, "header"), "slots");
    gint index = 0;
    for (; index < json_array_size(arr); index++) {
        json_t *j_type = json_object_get (json_array_get(arr, index), "type");
        json_int_t int_type = json_integer_value (j_type);
        if (int_type == 1) break;
    }
    json_t *wanted_obj = json_array_get (arr, index);
    gint n = (gint)json_integer_value (json_object_get (wanted_obj, "n"));
    gint p = (gint)json_integer_value (json_object_get (wanted_obj, "p"));
    guchar *salt = hexstr_to_bytes (json_string_value (json_object_get (wanted_obj, "salt")));
    guchar *enc_key = hexstr_to_bytes(json_string_value (json_object_get (wanted_obj, "key")));
    json_t *kp = json_object_get (wanted_obj, "key_params");
    guchar *key_nonce = hexstr_to_bytes (json_string_value (json_object_get (kp, "nonce")));
    guchar *key_tag = hexstr_to_bytes (json_string_value (json_object_get (kp, "tag")));
    json_t *dbp = json_object_get(json_object_get(json, "header"), "params");
    guchar *keybuf = gcry_malloc (KEY_SIZE);
    if (gcry_kdf_derive (password, g_utf8_strlen (password, -1), GCRY_KDF_SCRYPT, n, salt, SALT_SIZE,  p, KEY_SIZE, keybuf) != 0) {
        g_printerr ("Error while deriving the key.\n");
        g_free (salt);
        g_free (enc_key);
        g_free (key_nonce);
        g_free (key_tag);
        gcry_free (keybuf);
        json_decref (json);
        return NULL;
    }

    gcry_cipher_hd_t hd = open_cipher_and_set_data (keybuf, key_nonce, NONCE_SIZE);
    if (hd == NULL) {
        g_free (salt);
        g_free (enc_key);
        g_free (key_nonce);
        g_free (key_tag);
        gcry_free (keybuf);
        json_decref (json);
        return NULL;
    }

    guchar *master_key = gcry_calloc_secure (KEY_SIZE, 1);
    if (gcry_cipher_decrypt (hd, master_key, KEY_SIZE, enc_key, KEY_SIZE) != 0) {
        g_printerr ("Error while decrypting the master key.\n");
        g_free (salt);
        g_free (enc_key);
        g_free (key_nonce);
        g_free (key_tag);
        gcry_free (master_key);
        gcry_free (keybuf);
        gcry_cipher_close (hd);
        json_decref (json);
        return NULL;
    }
    gpg_error_t gpg_err = gcry_cipher_checktag (hd, key_tag, TAG_SIZE);
    if (gpg_err != 0) {
        g_set_error (err, bad_tag_gquark (), BAD_TAG_ERRCODE, "Invalid TAG (master key). Either the password is wrong or the file is corrupted.");
        g_free (salt);
        g_free (enc_key);
        g_free (key_nonce);
        g_free (key_tag);
        gcry_free (master_key);
        gcry_free (keybuf);
        gcry_cipher_close (hd);
        json_decref (json);
        return NULL;
    }

    g_free (salt);
    g_free (enc_key);
    g_free (key_nonce);
    g_free (key_tag);
    gcry_free (keybuf);
    gcry_cipher_close (hd);

    guchar *nonce = hexstr_to_bytes (json_string_value (json_object_get (dbp, "nonce")));
    guchar *tag = hexstr_to_bytes (json_string_value (json_object_get (dbp, "tag")));

    hd = open_cipher_and_set_data (master_key, nonce, 12);
    if (hd == NULL) {
        g_free (tag);
        g_free (nonce);
        gcry_free (master_key);
        json_decref (json);
        return NULL;
    }

    gsize out_len;
    guchar *b64decoded_db = g_base64_decode (json_string_value (json_object_get (json, "db")), &out_len);
    if (out_len > max_file_size) {
        g_set_error (err, file_too_big_gquark (), FILE_TOO_BIG, "File is too big");
        g_free (tag);
        g_free (nonce);
        gcry_free (master_key);
        g_free (b64decoded_db);
        gcry_cipher_close (hd);
        json_decref (json);
        return NULL;
    }
    // we no longer need the json object, so we can free up some secure memory
    json_decref (json);

    gchar *decrypted_db = gcry_calloc_secure (out_len, 1);
    gpg_err = gcry_cipher_decrypt (hd, decrypted_db, out_len, b64decoded_db, out_len);
    if (gpg_err) {
        goto clean_and_exit;
    }
    gpg_err = gcry_cipher_checktag (hd, tag, TAG_SIZE);
    if (gpg_err != 0) {
        g_set_error (err, bad_tag_gquark (), BAD_TAG_ERRCODE, "Invalid TAG (database). Either the password is wrong or the file is corrupted.");
        clean_and_exit:
        g_free (b64decoded_db);
        g_free (nonce);
        g_free (tag);
        gcry_free (master_key);
        gcry_free (decrypted_db);
        gcry_cipher_close (hd);
        return NULL;
    }

    g_free (b64decoded_db);
    g_free (nonce);
    g_free (tag);
    gcry_cipher_close (hd);
    gcry_free (master_key);

    // we remove the icon field (and the icon_mime while at it too) because it uses lots of secure memory for nothing
    GRegex *regex = g_regex_new (".*\"icon\":(\\s)*\".*\",\\n|.*\"icon_mime\":(\\s)*\".*\",\\n", G_REGEX_MULTILINE, 0, NULL);
    gchar *cleaned_db = secure_strdup (g_regex_replace (regex, decrypted_db, -1, 0, "", 0, NULL));
    g_regex_unref (regex);
    gcry_free (decrypted_db);

    GSList *otps = parse_json_data (cleaned_db, err);
    gcry_free (cleaned_db);

    return otps;
}


gchar *
export_aegis (const gchar   *export_path,
              json_t        *json_db_data,
              const gchar   *password)
{
    GError *err = NULL;
    json_t *root = json_object ();
    json_object_set (root, "version", json_integer (1));

    gcry_cipher_hd_t hd;
    guchar *derived_master_key = NULL, *enc_master_key = NULL, *key_nonce = NULL, *key_tag = NULL, *db_nonce = NULL, *db_tag = NULL, *salt = NULL;
    json_t *aegis_header_obj = json_object ();
    if (password == NULL) {
        json_object_set (aegis_header_obj, "slots", json_null ());
        json_object_set (aegis_header_obj, "params", json_null ());
    } else {
        json_t *slots_arr = json_array();
        json_t *slot_1 = json_object();
        json_array_append (slots_arr, slot_1);
        json_object_set (slot_1, "type", json_integer (1));

        uuid_t binuuid;
        uuid_generate_random (binuuid);
        gchar *uuid = g_malloc0 (37);
        uuid_unparse_lower (binuuid, uuid);
        json_object_set (slot_1, "uuid", json_string (g_strdup (uuid)));
        g_free (uuid);

        salt = g_malloc0 (SALT_SIZE);
        gcry_create_nonce (salt, SALT_SIZE);

        key_nonce = g_malloc0 (NONCE_SIZE);
        gcry_create_nonce (key_nonce, NONCE_SIZE);

        derived_master_key = gcry_calloc_secure(KEY_SIZE, 1);
        gpg_error_t gpg_err = gcry_kdf_derive (password, g_utf8_strlen (password, -1), GCRY_KDF_SCRYPT, 32768, salt, SALT_SIZE,  1, KEY_SIZE, derived_master_key);
        if (gpg_err) {
            g_printerr ("Error while deriving the key\n");
            gcry_free (derived_master_key);
            return NULL;
        }

        hd = open_cipher_and_set_data (derived_master_key, key_nonce, NONCE_SIZE);
        if (hd == NULL) {
            gcry_free (derived_master_key);
            g_free (key_nonce);
            g_free (salt);
            return NULL;
        }

        enc_master_key = gcry_malloc (KEY_SIZE);
        if (gcry_cipher_encrypt (hd, enc_master_key, KEY_SIZE, derived_master_key, KEY_SIZE)) {
            g_printerr ("Error while encrypting the master key.\n");
            gcry_free (derived_master_key);
            gcry_free (enc_master_key);
            g_free (key_nonce);
            g_free (salt);
            gcry_cipher_close (hd);
            return NULL;
        }

        key_tag = g_malloc0 (TAG_SIZE);
        gcry_cipher_gettag (hd, key_tag, TAG_SIZE);
        json_object_set (slot_1, "key", json_string (bytes_to_hexstr (enc_master_key, KEY_SIZE)));
        gcry_cipher_close (hd);

        json_t *kp = json_object();
        json_object_set (kp, "nonce", json_string(bytes_to_hexstr (key_nonce, NONCE_SIZE)));
        json_object_set (kp, "tag", json_string (bytes_to_hexstr (key_tag, TAG_SIZE)));
        json_object_set (slot_1, "key_params", kp);
        json_object_set (slot_1, "n", json_integer (32768));
        json_object_set (slot_1, "r", json_integer (8));
        json_object_set (slot_1, "p", json_integer (1));
        json_object_set (slot_1, "salt", json_string (bytes_to_hexstr (salt, SALT_SIZE)));
        json_object_set (aegis_header_obj, "slots", slots_arr);

        json_t *db_params_obj = json_object();
        db_nonce = g_malloc0 (NONCE_SIZE);
        gcry_create_nonce (db_nonce, NONCE_SIZE);
        json_object_set (db_params_obj, "nonce", json_string (bytes_to_hexstr (db_nonce, NONCE_SIZE)));

        db_tag = g_malloc0 (TAG_SIZE);
        // tag is computed after encryption, so we just put a placeholder here
        json_object_set (db_params_obj, "tag", json_null ());
        json_object_set (aegis_header_obj, "params", db_params_obj);
    }
    json_object_set (root, "header", aegis_header_obj);

    json_t *aegis_db_obj = json_object ();
    json_t *array = json_array ();
    json_object_set (aegis_db_obj, "version", json_integer (2));
    json_object_set (aegis_db_obj, "entries", array);
    json_object_set (root, "db", aegis_db_obj);

    json_t *db_obj, *export_obj, *info_obj;
    gsize index;
    json_array_foreach (json_db_data, index, db_obj) {
        export_obj = json_object ();
        info_obj = json_object ();
        json_t *otp_type = json_object_get (db_obj, "type");

        const gchar *issuer = json_string_value (json_object_get (db_obj, "issuer"));
        if (issuer != NULL && g_ascii_strcasecmp (issuer, "steam") == 0) {
            json_object_set (export_obj, "type", json_string ("steam"));
        } else {
            json_object_set (export_obj, "type", json_string (g_utf8_strdown (json_string_value (otp_type), -1)));
        }

        json_object_set (export_obj, "name", json_object_get (db_obj, "label"));
        const gchar *issuer_from_db = json_string_value (json_object_get (db_obj, "issuer"));
        if (issuer_from_db != NULL && g_utf8_strlen (issuer_from_db, -1) > 0) {
            json_object_set (export_obj, "issuer", json_string (issuer_from_db));
        } else {
            json_object_set (export_obj, "issuer", json_null ());
        }

        json_object_set (export_obj, "icon", json_null ());

        json_object_set (info_obj, "secret", json_object_get (db_obj, "secret"));
        json_object_set (info_obj, "digits", json_object_get (db_obj, "digits"));
        json_object_set (info_obj, "algo", json_object_get (db_obj, "algo"));
        if (g_ascii_strcasecmp (json_string_value (otp_type), "TOTP") == 0) {
            json_object_set (info_obj, "period", json_object_get (db_obj, "period"));
        } else {
            json_object_set (info_obj, "counter", json_object_get (db_obj, "counter"));
        }

        json_object_set (export_obj, "info", info_obj);

        json_array_append (array, export_obj);
    }

    if (password != NULL) {
        hd = open_cipher_and_set_data (derived_master_key, db_nonce, NONCE_SIZE);
        if (hd == NULL) {
            goto clean_and_return;
        }
        size_t db_size = json_dumpb (aegis_db_obj, NULL, 0, 0);
        guchar *enc_db = g_malloc0 (db_size);
        gchar *dumped_db = g_malloc0 (db_size);
        json_dumpb (aegis_db_obj, dumped_db, db_size, 0);
        if (gcry_cipher_encrypt (hd, enc_db, db_size, dumped_db, db_size)) {
            g_printerr ("Error while encrypting the db.\n");
            g_free (enc_db);
            g_free (dumped_db);
            gcry_cipher_close (hd);
            clean_and_return:
            g_free (key_nonce);
            g_free (key_tag);
            g_free (db_nonce);
            g_free (db_tag);
            g_free (salt);
            gcry_free (derived_master_key);
            gcry_free (enc_master_key);
            return NULL;
        }
        gcry_cipher_gettag (hd, db_tag, TAG_SIZE);
        json_t *db_params = json_object_get (aegis_header_obj, "params");
        json_object_set (db_params, "tag", json_string (bytes_to_hexstr (db_tag, TAG_SIZE)));
        g_free (dumped_db);
        gchar *b64enc_db = g_base64_encode (enc_db, db_size);
        json_object_set (root, "db", json_string (b64enc_db));

        g_free (b64enc_db);
        g_free (enc_db);
        g_free (key_nonce);
        g_free (key_tag);
        g_free (db_nonce);
        g_free (db_tag);
        g_free (salt);
        gcry_free (derived_master_key);
        gcry_free (enc_master_key);
        gcry_cipher_close (hd);
    }

    GFile *out_gfile = g_file_new_for_path (export_path);
    GFileOutputStream *out_stream = g_file_replace (out_gfile, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION | G_FILE_CREATE_PRIVATE, NULL, &err);
    if (err == NULL) {
        gsize jbuf_size = json_dumpb (root, NULL, 0, 0);
        if (jbuf_size == 0) {
            goto cleanup_and_exit;
        }
        gchar *jbuf = g_malloc0 (jbuf_size);
        if (json_dumpb (root, jbuf, jbuf_size, JSON_COMPACT) == -1) {
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "couldn't dump json data to buffer");
            g_free (jbuf);
            goto cleanup_and_exit;
        }
        if (g_output_stream_write (G_OUTPUT_STREAM(out_stream), jbuf, jbuf_size, NULL, &err) == -1) {
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "couldn't dump json data to file");
            g_free (jbuf);
            goto cleanup_and_exit;
        }
        g_free (jbuf);
    } else {
        g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "couldn't create the file object");
    }

    cleanup_and_exit:
    json_array_clear (array);
    json_decref (aegis_db_obj);
    json_decref (aegis_header_obj);
    json_decref (root);
    g_object_unref (out_stream);
    g_object_unref (out_gfile);

    return (err != NULL ? g_strdup (err->message) : NULL);
}


static GSList *
parse_json_data (const gchar *data,
                 GError     **err)
{
    json_error_t jerr;
    json_t *root = json_loads (data, JSON_DISABLE_EOF_CHECK, &jerr);
    if (root == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "%s", jerr.text);
        return NULL;
    }

    json_t *array = json_object_get (root, "entries");
    if (array == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "%s", jerr.text);
        json_decref (root);
        return NULL;
    }

    GSList *otps = NULL;
    for (guint i = 0; i < json_array_size (array); i++) {
        json_t *obj = json_array_get (array, i);

        otp_t *otp = g_new0 (otp_t, 1);
        otp->issuer = g_strdup (json_string_value (json_object_get (obj, "issuer")));
        otp->account_name = g_strdup (json_string_value (json_object_get (obj, "name")));

        json_t *info_obj = json_object_get (obj, "info");
        otp->secret = secure_strdup (json_string_value (json_object_get (info_obj, "secret")));
        otp->digits = (guint32) json_integer_value (json_object_get(info_obj, "digits"));

        const gchar *type = json_string_value (json_object_get (obj, "type"));
        if (g_ascii_strcasecmp (type, "TOTP") == 0) {
            otp->type = g_strdup (type);
            otp->period = (guint32)json_integer_value (json_object_get (info_obj, "period"));
        } else if (g_ascii_strcasecmp (type, "HOTP") == 0) {
            otp->type = g_strdup (type);
            otp->counter = json_integer_value (json_object_get (info_obj, "counter"));
        } else if (g_ascii_strcasecmp (type, "Steam") == 0) {
            otp->type = g_strdup ("TOTP");
            otp->period = (guint32)json_integer_value (json_object_get (info_obj, "period"));
            if (otp->period == 0) {
                // Aegis exported backup for Steam might not contain the period field,
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

        const gchar *algo = json_string_value (json_object_get (info_obj, "algo"));
        if (g_ascii_strcasecmp (algo, "SHA1") == 0 ||
            g_ascii_strcasecmp (algo, "SHA256") == 0 ||
            g_ascii_strcasecmp (algo, "SHA512") == 0) {
                otp->algo = g_ascii_strup (algo, -1);
        } else {
            g_printerr ("algo not supported (must be either one of: sha1, sha256 or sha512\n");
            gcry_free (otp->secret);
            g_free (otp);
            json_decref (obj);
            json_decref (info_obj);
            return NULL;
        }

        otps = g_slist_append (otps, g_memdupX (otp, sizeof (otp_t)));
        g_free (otp);
    }

    json_decref (root);

    return otps;
}
