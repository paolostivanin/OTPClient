#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <gcrypt.h>
#include <jansson.h>
#include <time.h>
#include <uuid/uuid.h>
#include "gquarks.h"
#include "common.h"
#include "file-size.h"
#include "parse-uri.h"


#define AEGIS_NONCE_SIZE  12
#define AEGIS_TAG_SIZE    16
#define AEGIS_SALT_SIZE   32
#define AEGIS_KEY_SIZE    32


static GSList   *get_otps_from_plain_backup     (const gchar  *path,
                                                 GError      **err);

static GSList   *get_otps_from_encrypted_backup (const gchar  *path,
                                                 const gchar  *password,
                                                 gint32        max_file_size,
                                                 GError      **err);

static GSList   *parse_aegis_json_data          (const gchar  *data,
                                                 GError      **err);

static gboolean  is_file_otpauth_txt            (const gchar  *file_path,
                                                 GError      **err);

static gchar    *remove_icons_from_db           (const gchar  *decrypted_db,
                                                 gboolean      use_secure_memory);


GSList *
get_aegis_data (const gchar     *path,
                const gchar     *password,
                gint32           max_file_size,
                gsize            db_size,
                GError         **err)
{
    if (g_file_test (path, G_FILE_TEST_IS_SYMLINK | G_FILE_TEST_IS_DIR) ) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Selected file is either a symlink or a directory.");
        return NULL;
    }

    goffset input_size = get_file_size (path);
    if (!is_secmem_available ((db_size + input_size)  * SECMEM_REQUIRED_MULTIPLIER, err)) {
        g_autofree gchar *msg = g_strdup_printf (_(
            "Your system's secure memory limit is not enough to securely import the data.\n"
            "You need to increase your system's memlock limit by following the instructions on our "
            "<a href=\"https://github.com/paolostivanin/OTPClient/wiki/Secure-Memory-Limitations\">secure memory wiki page</a>.\n"
            "This requires administrator privileges and is a system-wide setting that OTPClient cannot change automatically."
        ));
        g_set_error (err, secmem_alloc_error_gquark (), NO_SECMEM_AVAIL_ERRCODE, "%s", msg);
        return NULL;
    }

    return (password != NULL) ? get_otps_from_encrypted_backup (path, password, max_file_size, err) : get_otps_from_plain_backup (path, err);
}


static GSList *
get_otps_from_plain_backup (const gchar  *path,
                           GError      **err)
{
    GSList *otps = NULL;
    if (is_file_otpauth_txt (path, err)) {
        gint32 max_file_size = 0;
        set_memlock_value (&max_file_size);
        otps = get_otpauth_data (path, max_file_size, err);
    } else {
        // Due to icons, custom icons, etc, loading the entire json into secure memory could drain the pool and could cause
        // the app to segfault. Since the file is unencrypted, we don't need to load it into secure memory.
        json_set_alloc_funcs (g_malloc0, g_free);
        json_error_t j_err;
        json_t *json = json_load_file (path, JSON_DISABLE_EOF_CHECK | JSON_ALLOW_NUL, &j_err);
        if (!json) {
            gchar *msg = g_strconcat ("Error while loading the json file: ", j_err.text, NULL);
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "%s", msg);
            g_free (msg);
            json_set_alloc_funcs (gcry_malloc_secure, gcry_free);
            return NULL;
        }

        gchar *dumped_json = json_dumps (json_object_get (json, "db"), 0);
        gchar *cleaned_db = remove_icons_from_db (dumped_json, FALSE);
        g_free (dumped_json);

        otps = parse_aegis_json_data (cleaned_db, err);
        g_free (cleaned_db);
        json_set_alloc_funcs (gcry_malloc_secure, gcry_free);
    }
    return otps;
}


static GSList *
get_otps_from_encrypted_backup (const gchar          *path,
                                const gchar          *password,
                                gint32                max_file_size,
                                GError              **err)
{
    // Due to icons, custom icons, etc, loading the entire json into secure memory could drain the pool and could cause
    // the app to segfault. Since we only need the decrypted data to be handled in secure memory, we can use the standard memory
    // for the other data.
    json_set_alloc_funcs (g_malloc0, g_free);

    json_error_t j_err;
    json_t *json = json_load_file (path, JSON_DISABLE_EOF_CHECK | JSON_ALLOW_NUL, &j_err);
    if (!json) {
        g_printerr ("Error loading json: %s\n", j_err.text);
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Error while loading the Aegis backup: %s", j_err.text);
        json_set_alloc_funcs (gcry_malloc_secure, gcry_free);
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
    guchar *keybuf = gcry_malloc (AEGIS_KEY_SIZE);
    if (gcry_kdf_derive (password, g_utf8_strlen (password, -1), GCRY_KDF_SCRYPT, n, salt, AEGIS_SALT_SIZE,  p, AEGIS_KEY_SIZE, keybuf) != 0) {
        g_printerr ("Error while deriving the key.\n");
        g_set_error (err, key_deriv_gquark (), KEY_DERIVATION_ERRCODE, "Error while deriving the Aegis decryption key.");
        g_free (salt);
        g_free (enc_key);
        g_free (key_nonce);
        g_free (key_tag);
        gcry_free (keybuf);
        json_decref (json);
        json_set_alloc_funcs (gcry_malloc_secure, gcry_free);
        return NULL;
    }

    gcry_cipher_hd_t hd = open_cipher_and_set_data (keybuf, key_nonce, AEGIS_NONCE_SIZE);
    if (hd == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Error while opening the Aegis cipher handle.");
        g_free (salt);
        g_free (enc_key);
        g_free (key_nonce);
        g_free (key_tag);
        gcry_free (keybuf);
        json_decref (json);
        json_set_alloc_funcs (gcry_malloc_secure, gcry_free);
        return NULL;
    }

    guchar *master_key = gcry_calloc_secure (AEGIS_KEY_SIZE, 1);
    if (gcry_cipher_decrypt (hd, master_key, AEGIS_KEY_SIZE, enc_key, AEGIS_KEY_SIZE) != 0) {
        g_printerr ("Error while decrypting the master key.\n");
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Error while decrypting the Aegis master key.");
        g_free (salt);
        g_free (enc_key);
        g_free (key_nonce);
        g_free (key_tag);
        gcry_free (master_key);
        gcry_free (keybuf);
        gcry_cipher_close (hd);
        json_decref (json);
        json_set_alloc_funcs (gcry_malloc_secure, gcry_free);
        return NULL;
    }
    gpg_error_t gpg_err = gcry_cipher_checktag (hd, key_tag, AEGIS_TAG_SIZE);
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
        json_set_alloc_funcs (gcry_malloc_secure, gcry_free);
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
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Error while opening the Aegis database cipher handle.");
        g_free (tag);
        g_free (nonce);
        gcry_free (master_key);
        json_decref (json);
        json_set_alloc_funcs (gcry_malloc_secure, gcry_free);
        return NULL;
    }

    gsize out_len;
    guchar *b64decoded_db = g_base64_decode (json_string_value (json_object_get (json, "db")), &out_len);
    if (out_len > (gint32)(max_file_size * SECMEM_SIZE_THRESHOLD_RATIO)) {
        // Input data is too big, so we don't load it into secure memory
        g_set_error (err, file_too_big_gquark (), FILE_TOO_BIG_ERRCODE, FILE_SIZE_SECMEM_MSG);
        g_free (tag);
        g_free (nonce);
        gcry_free (master_key);
        g_free (b64decoded_db);
        gcry_cipher_close (hd);
        json_decref (json);
        json_set_alloc_funcs (gcry_malloc_secure, gcry_free);
        return NULL;
    }
    // we no longer need the json object, so we can free up some secure memory
    json_decref (json);
    json_set_alloc_funcs (gcry_malloc_secure, gcry_free);

    gchar *decrypted_db = gcry_calloc_secure (out_len, 1);
    gpg_err = gcry_cipher_decrypt (hd, decrypted_db, out_len, b64decoded_db, out_len);
    if (gpg_err) {
        goto clean_and_exit;
    }
    gpg_err = gcry_cipher_checktag (hd, tag, AEGIS_TAG_SIZE);
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

    gchar *cleaned_db = remove_icons_from_db (decrypted_db, TRUE);
    GSList *otps = parse_aegis_json_data (cleaned_db, err);
    gcry_free (cleaned_db);

    return otps;
}


gchar *
export_aegis (const gchar   *export_path,
              const gchar   *password,
              json_t        *json_db_data)
{
    GError *err = NULL;
    gsize db_size = json_dumpb (json_db_data, NULL, 0, 0);
    if (!is_secmem_available (db_size * SECMEM_REQUIRED_MULTIPLIER, &err)) {
        g_autofree gchar *msg = g_strdup_printf (_(
            "Your system's secure memory limit is not enough to securely export the database.\n"
            "You need to increase your system's memlock limit by following the instructions on our "
            "<a href=\"https://github.com/paolostivanin/OTPClient/wiki/Secure-Memory-Limitations\">secure memory wiki page</a>.\n"
            "This requires administrator privileges and is a system-wide setting that OTPClient cannot change automatically."
        ));
        g_set_error (&err, secmem_alloc_error_gquark (), NO_SECMEM_AVAIL_ERRCODE, "%s", msg);
        return NULL;
    }

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

        salt = g_malloc0 (AEGIS_SALT_SIZE);
        gcry_create_nonce (salt, AEGIS_SALT_SIZE);

        key_nonce = g_malloc0 (AEGIS_NONCE_SIZE);
        gcry_create_nonce (key_nonce, AEGIS_NONCE_SIZE);

        derived_master_key = gcry_calloc_secure(AEGIS_KEY_SIZE, 1);
        gpg_error_t gpg_err = gcry_kdf_derive (password, g_utf8_strlen (password, -1), GCRY_KDF_SCRYPT, 32768, salt, AEGIS_SALT_SIZE,  1, AEGIS_KEY_SIZE, derived_master_key);
        if (gpg_err) {
            g_printerr ("Error while deriving the key\n");
            gcry_free (derived_master_key);
            g_free (key_nonce);
            g_free (salt);
            return NULL;
        }

        hd = open_cipher_and_set_data (derived_master_key, key_nonce, AEGIS_NONCE_SIZE);
        if (hd == NULL) {
            gcry_free (derived_master_key);
            g_free (key_nonce);
            g_free (salt);
            return NULL;
        }

        enc_master_key = gcry_malloc (AEGIS_KEY_SIZE);
        if (gcry_cipher_encrypt (hd, enc_master_key, AEGIS_KEY_SIZE, derived_master_key, AEGIS_KEY_SIZE)) {
            g_printerr ("Error while encrypting the master key.\n");
            gcry_free (derived_master_key);
            gcry_free (enc_master_key);
            g_free (key_nonce);
            g_free (salt);
            gcry_cipher_close (hd);
            return NULL;
        }

        key_tag = g_malloc0 (AEGIS_TAG_SIZE);
        gcry_cipher_gettag (hd, key_tag, AEGIS_TAG_SIZE);
        json_object_set (slot_1, "key", json_string (bytes_to_hexstr (enc_master_key, AEGIS_KEY_SIZE)));
        gcry_cipher_close (hd);

        json_t *kp = json_object();
        json_object_set (kp, "nonce", json_string(bytes_to_hexstr (key_nonce, AEGIS_NONCE_SIZE)));
        json_object_set (kp, "tag", json_string (bytes_to_hexstr (key_tag, AEGIS_TAG_SIZE)));
        json_object_set (slot_1, "key_params", kp);
        json_object_set (slot_1, "n", json_integer (32768));
        json_object_set (slot_1, "r", json_integer (8));
        json_object_set (slot_1, "p", json_integer (1));
        json_object_set (slot_1, "salt", json_string (bytes_to_hexstr (salt, AEGIS_SALT_SIZE)));
        json_object_set (aegis_header_obj, "slots", slots_arr);

        json_t *db_params_obj = json_object();
        db_nonce = g_malloc0 (AEGIS_NONCE_SIZE);
        gcry_create_nonce (db_nonce, AEGIS_NONCE_SIZE);
        json_object_set (db_params_obj, "nonce", json_string (bytes_to_hexstr (db_nonce, AEGIS_NONCE_SIZE)));

        db_tag = g_malloc0 (AEGIS_TAG_SIZE);
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
        hd = open_cipher_and_set_data (derived_master_key, db_nonce, AEGIS_NONCE_SIZE);
        if (hd == NULL) {
            goto clean_and_return;
        }
        db_size = json_dumpb (aegis_db_obj, NULL, 0, 0);
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
        gcry_cipher_gettag (hd, db_tag, AEGIS_TAG_SIZE);
        json_t *db_params = json_object_get (aegis_header_obj, "params");
        json_object_set (db_params, "tag", json_string (bytes_to_hexstr (db_tag, AEGIS_TAG_SIZE)));
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
parse_aegis_json_data (const gchar *data,
                       GError     **err)
{
    json_error_t jerr;
    json_t *root = json_loads (data, JSON_DISABLE_EOF_CHECK | JSON_ALLOW_NUL, &jerr);
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

        gboolean skip = FALSE;
        const gchar *type = json_string_value (json_object_get (obj, "type"));
        if (type == NULL) {
            g_printerr ("Skipping token due to missing type field\n");
            skip = TRUE;
        } else if (g_ascii_strcasecmp (type, "TOTP") == 0) {
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
            g_printerr ("Skipping token due to unsupported type: %s\n", type);
            skip = TRUE;
        }

        const gchar *algo = json_string_value (json_object_get (info_obj, "algo"));
        if (algo == NULL) {
            g_printerr ("Skipping token due to missing algo field\n");
            skip = TRUE;
        } else if (g_ascii_strcasecmp (algo, "SHA1") == 0 ||
            g_ascii_strcasecmp (algo, "SHA256") == 0 ||
            g_ascii_strcasecmp (algo, "SHA512") == 0) {
                otp->algo = g_ascii_strup (algo, -1);
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

    json_decref (root);

    return otps;
}


static gboolean
is_file_otpauth_txt (const gchar  *file_path,
                     GError      **err)
{
    gboolean result = FALSE;

    GFile *file = g_file_new_for_path (file_path);
    GFileInputStream *input_stream = g_file_read (file, NULL, err);
    if (err != NULL && *err != NULL) {
        g_object_unref (file);
        return result;
    }

    const gchar *expected_string = "otpauth://";
    gsize expected_string_len = g_utf8_strlen (expected_string, -1);
    gchar buffer[expected_string_len + 1];
    gssize bytes_read = g_input_stream_read (G_INPUT_STREAM(input_stream), buffer, expected_string_len, NULL, err);
    if ((err == NULL || *err == NULL) && bytes_read == expected_string_len) {
        buffer[expected_string_len] = '\0';
        result = (g_strcmp0 (buffer, expected_string) == 0);
        g_object_unref (input_stream);
    }
    g_object_unref (file);

    return result;
}


static gchar *
remove_icons_from_db (const gchar *decrypted_db,
                      gboolean     use_secure_memory)
{
    typedef gchar* (*StrDupFunc)(const gchar*);
    StrDupFunc strdup_func = use_secure_memory == TRUE ? secure_strdup : g_strdup;

    // we remove the icon field (and the icon_mime while at it too) because it uses lots of secure memory for nothing
    GRegex *regex = g_regex_new (".*\"icon\":(\\s)*\".*\",\\n|.*\"icon_mime\":(\\s)*\".*\",\\n", G_REGEX_MULTILINE, 0, NULL);
    gchar *cleaned_db = strdup_func (g_regex_replace (regex, decrypted_db, -1, 0, "", 0, NULL));
    g_regex_unref (regex);

    return cleaned_db;
}
