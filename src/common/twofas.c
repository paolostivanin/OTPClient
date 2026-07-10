#define _DEFAULT_SOURCE
#define _GNU_SOURCE
#include <string.h>
#include <stdlib.h>
#include <glib.h>
#include <gio/gio.h>
#include <jansson.h>
#include <gcrypt.h>
#include <unistd.h>
#include <glib/gi18n.h>

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

static gboolean  is_schema_supported            (const gchar       *path,
                                                 GError           **err);

static gboolean  decrypt_data                   (const gchar      **b64_data,
                                                 const gchar       *pwd,
                                                 TwofasData        *twofas_data,
                                                 GError           **err);

static gchar    *get_encoded_data               (guchar            *enc_buf,
                                                 gsize              enc_buf_len,
                                                 guchar            *salt,
                                                 guchar            *iv);

static gchar    *get_reference_data             (guchar            *derived_key,
                                                 guchar            *salt);

static GSList   *parse_twofas_json_data         (const gchar       *data,
                                                 GHashTable        *group_map,
                                                 GError           **err);


GSList *
get_twofas_data (const gchar  *path,
                 const gchar  *password,
                 gsize         db_size,
                 GError      **err)
{
    int safe_fd = path_open_safe_regular_file (path, err);
    if (safe_fd < 0) {
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
        g_clear_error (err);
        g_set_error (err, secmem_alloc_error_gquark (), NO_SECMEM_AVAIL_ERRCODE, "%s", msg);
        close (safe_fd);
        return NULL;
    }

    g_autofree gchar *fd_path = g_strdup_printf ("/proc/self/fd/%d", safe_fd);
    GSList *otps = (password != NULL) ? get_otps_from_encrypted_backup (fd_path, password, err)
                                       : get_otps_from_plain_backup (fd_path, err);
    close (safe_fd);
    return otps;
}


gchar *
export_twofas (const gchar *export_path,
               const gchar *password,
               json_t      *json_db_data)
{
    GError *err = NULL;
    json_t *root = NULL;
    json_t *enc_root = NULL;
    json_t *services_array = NULL;
    json_t *groups_array = NULL;
    GFile *out_gfile = NULL;
    GFileOutputStream *out_stream = NULL;
    gchar *json_data = NULL;
    gchar *json_enc_data = NULL;
    gchar *encoded_data = NULL;
    gchar *encoded_ref_data = NULL;
    guchar *salt = NULL, *iv = NULL;
    guchar *derived_key = NULL;
    guchar *enc_buf = NULL;
    guchar *enc_data_with_tag = NULL;
    gcry_cipher_hd_t hd = NULL;

    gsize db_size = json_dumpb (json_db_data, NULL, 0, 0);
    if (!is_secmem_available (db_size * SECMEM_REQUIRED_MULTIPLIER, &err)) {
        g_autofree gchar *msg = g_strdup_printf (_(
            "Your system's secure memory limit is not enough to securely export the database.\n"
            "You need to increase your system's memlock limit by following the instructions on our "
            "<a href=\"https://github.com/paolostivanin/OTPClient/wiki/Secure-Memory-Limitations\">secure memory wiki page</a>.\n"
            "This requires administrator privileges and is a system-wide setting that OTPClient cannot change automatically."
        ));
        g_clear_error (&err);
        g_set_error (&err, secmem_alloc_error_gquark (), NO_SECMEM_AVAIL_ERRCODE, "%s", msg);
        return g_strdup (err->message);
    }

    gint64 epoch_time = g_get_real_time();

    root = json_object ();
    services_array = json_array ();
    json_object_set (root, "services", services_array);
    /* services_array deliberately kept with refcount=2 (1 local + 1 root):
     * we json_dump it standalone for the encrypted branch below, then decref
     * our local handle in the cleanup. */

    /* Build groups array and group name -> UUID map */
    groups_array = json_array ();
    GHashTable *group_id_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    {
        gsize idx;
        json_t *tmp_obj;
        guint grp_counter = 0;
        json_array_foreach (json_db_data, idx, tmp_obj) {
            const gchar *group = json_string_value (json_object_get (tmp_obj, "group"));
            if (group != NULL && group[0] != '\0' && !g_hash_table_contains (group_id_map, group)) {
                g_autofree gchar *grp_id = g_strdup_printf ("grp-%u", grp_counter++);
                g_hash_table_insert (group_id_map, g_strdup (group), g_strdup (grp_id));
                json_t *grp_obj = json_object ();
                json_object_set_new (grp_obj, "id", json_string (grp_id));
                json_object_set_new (grp_obj, "name", json_string (group));
                json_array_append_new (groups_array, grp_obj);
            }
        }
    }
    /* Same as services_array: refcount=2 so we can reuse it under enc_root
     * in the encrypted branch without it being prematurely freed. */
    json_object_set (root, "groups", groups_array);
    json_object_set_new (root, "updatedAt", json_integer (epoch_time));
    json_object_set_new (root, "schemaVersion", json_integer (4));

    json_t *db_obj;
    gsize index;
    json_array_foreach (json_db_data, index, db_obj) {
        json_t *export_obj = json_object ();
        json_t *otp_obj = json_object ();
        json_t *order_obj = json_object ();
        const gchar *issuer = json_string_value (json_object_get (db_obj, "issuer"));
        if (issuer != NULL) {
            if (g_ascii_strcasecmp (issuer, "steam") == 0) {
                json_object_set_new (export_obj, "name", json_string ("Steam"));
                json_object_set_new (otp_obj, "issuer", json_string ("Steam"));
                json_object_set_new (otp_obj, "tokenType", json_string ("STEAM"));
            } else {
                json_object_set_new (export_obj, "name", json_string (issuer));
                json_object_set_new (otp_obj, "issuer", json_string (issuer));
            }
        }
        json_object_set_new (export_obj, "updatedAt", json_integer (epoch_time));
        json_object_set (export_obj, "secret", json_object_get (db_obj, "secret"));
        const gchar *label = json_string_value (json_object_get (db_obj, "label"));
        if (label != NULL) {
            json_object_set_new (otp_obj, "label", json_string (label));
            json_object_set_new (otp_obj, "account", json_string (label));
        }

        const gchar *algo_raw = json_string_value (json_object_get (db_obj, "algo"));
        if (algo_raw == NULL) algo_raw = "SHA1";
        gchar *algo = g_ascii_strup (algo_raw, -1);
        json_object_set_new (otp_obj, "algorithm", json_string (algo));
        g_free (algo);

        json_object_set (otp_obj, "digits", json_object_get (db_obj, "digits"));
        json_object_set_new (otp_obj, "source", json_string ("Manual"));

        const gchar *type_raw = json_string_value (json_object_get (db_obj, "type"));
        if (type_raw == NULL) type_raw = "TOTP";
        if (g_ascii_strcasecmp (type_raw, "TOTP") == 0) {
            json_object_set (otp_obj, "period", json_object_get (db_obj, "period"));
            json_object_set_new (otp_obj, "tokenType", json_string ("TOTP"));
        } else {
            json_object_set (otp_obj, "counter", json_object_get (db_obj, "counter"));
            json_object_set_new (otp_obj, "tokenType", json_string ("HOTP"));
        }

        json_object_set_new (order_obj, "position", json_integer ((json_int_t)index));
        json_object_set_new (export_obj, "otp", otp_obj);
        json_object_set_new (export_obj, "order", order_obj);

        const gchar *group = json_string_value (json_object_get (db_obj, "group"));
        if (group != NULL && group[0] != '\0') {
            const gchar *grp_id = g_hash_table_lookup (group_id_map, group);
            if (grp_id != NULL)
                json_object_set_new (export_obj, "groupId", json_string (grp_id));
        }

        json_array_append_new (services_array, export_obj);
    }
    g_hash_table_destroy (group_id_map);

    json_data = json_dumps ((password == NULL) ? root : services_array, JSON_COMPACT);
    if (json_data == NULL) {
        g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "Couldn't dump json data");
        goto end;
    }
    gsize json_data_size = strlen (json_data);

    out_gfile = g_file_new_for_path (export_path);
    out_stream = g_file_replace (out_gfile, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION | G_FILE_CREATE_PRIVATE, NULL, &err);
    if (out_stream == NULL) {
        // C1: g_file_replace already populated err; previously the code blindly
        // proceeded into the password branch and crashed on disk-full / R-O.
        goto end;
    }

    if (password != NULL) {
        salt = g_malloc0 (TWOFAS_SALT);
        gcry_create_nonce (salt, TWOFAS_SALT);
        iv = g_malloc0 (TWOFAS_IV);
        gcry_create_nonce (iv, TWOFAS_IV);
        derived_key = gcry_malloc_secure (32);
        if (derived_key == NULL) {
            g_set_error (&err, secmem_alloc_error_gquark (), SECMEM_ALLOC_ERRCODE,
                         "Couldn't allocate secure memory for the derived key.");
            goto end;
        }
        // gcry_kdf_derive expects the password length in BYTES, not Unicode characters.
        gpg_error_t g_err = gcry_kdf_derive (password, strlen (password), GCRY_KDF_PBKDF2, GCRY_MD_SHA256,
                                             salt, TWOFAS_SALT, TWOFAS_KDF_ITERS, 32, derived_key);
        if (g_err != GPG_ERR_NO_ERROR) {
            g_set_error (&err, key_deriv_gquark (), KEY_DERIVATION_ERRCODE,
                         "Error while deriving the key: %s/%s",
                         gcry_strsource (g_err), gcry_strerror (g_err));
            goto end;
        }
        hd = open_cipher_and_set_data (derived_key, iv, TWOFAS_IV);
        if (hd == NULL) {
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "Error while opening the cipher.");
            goto end;
        }
        enc_buf = g_malloc0 (json_data_size);
        gpg_error_t gpg_err = gcry_cipher_encrypt (hd, enc_buf, json_data_size, json_data, json_data_size);
        if (gpg_err != GPG_ERR_NO_ERROR) {
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE,
                         "Failed to encrypt data: %s/%s",
                         gcry_strsource (gpg_err), gcry_strerror (gpg_err));
            goto end;
        }
        guchar tag[TWOFAS_TAG];
        gcry_cipher_gettag (hd, tag, TWOFAS_TAG);
        gcry_cipher_close (hd);
        hd = NULL;

        enc_data_with_tag = g_malloc0 (json_data_size + TWOFAS_TAG);
        memcpy (enc_data_with_tag, enc_buf, json_data_size);
        memcpy (enc_data_with_tag + json_data_size, tag, TWOFAS_TAG);
        explicit_bzero (tag, TWOFAS_TAG);
        gcry_free (enc_buf);
        enc_buf = NULL;

        enc_root = json_object ();
        json_object_set_new (enc_root, "services", json_array ());
        // groups_array is already owned by `root` (refcount=2 from earlier).
        // Adding it under enc_root needs an explicit incref so refcount=3 keeps
        // both parents whole; cleanup decrefs both parents then our local handle.
        json_object_set (enc_root, "groups", groups_array);
        json_object_set_new (enc_root, "schemaVersion", json_integer (4));
        encoded_data = get_encoded_data (enc_data_with_tag, json_data_size + TWOFAS_TAG, salt, iv);
        json_object_set_new (enc_root, "servicesEncrypted", json_string (encoded_data));
        encoded_ref_data = get_reference_data (derived_key, salt);
        if (encoded_ref_data == NULL) {
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "Couldn't encrypt the reference data.");
            goto end;
        }
        // json_string() copies the C string, it does not take ownership, so
        // encoded_ref_data still has to be freed by us (done at `end:`).
        json_object_set_new (enc_root, "reference", json_string (encoded_ref_data));

        json_enc_data = json_dumps (enc_root, JSON_COMPACT);
        if (json_enc_data == NULL) {
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "Couldn't dump json data");
            goto end;
        }
        if (!output_stream_write_all_exact (G_OUTPUT_STREAM(out_stream), json_enc_data, strlen (json_enc_data), &err)) {
            // err is set by g_output_stream_write.
            goto end;
        }
    } else {
        // write the plain json to disk
        if (!output_stream_write_all_exact (G_OUTPUT_STREAM(out_stream), json_data, json_data_size, &err)) {
            // err is set by g_output_stream_write.
            goto end;
        }
    }

end:
    if (hd != NULL) gcry_cipher_close (hd);
    if (derived_key != NULL) {
        explicit_bzero (derived_key, 32);
        gcry_free (derived_key);
    }
    if (enc_buf != NULL) gcry_free (enc_buf);
    g_free (enc_data_with_tag);
    g_free (encoded_data);
    g_free (encoded_ref_data);
    g_free (iv);
    g_free (salt);
    if (json_enc_data != NULL) gcry_free (json_enc_data);
    if (json_data != NULL) gcry_free (json_data);
    if (enc_root != NULL) json_decref (enc_root);
    if (services_array != NULL) json_decref (services_array);
    if (groups_array != NULL) json_decref (groups_array);
    if (root != NULL) json_decref (root);
    if (out_stream != NULL) g_object_unref (out_stream);
    if (out_gfile != NULL) g_object_unref (out_gfile);

    return (err != NULL ? g_strdup (err->message) : NULL);
}


static GSList *
get_otps_from_encrypted_backup (const gchar       *path,
                                const gchar       *password,
                                GError           **err)
{
    if (!is_schema_supported (path, err)) {
        return NULL;
    }

    TwofasData *twofas_data = g_new0 (TwofasData, 1);
    GSList *otps = NULL;

    json_t *root = get_json_root (path);
    if (root == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Could not parse the 2FAS backup JSON.");
        g_free (twofas_data);
        return NULL;
    }
    const gchar *services_encrypted = json_string_value (json_object_get (root, "servicesEncrypted"));
    if (services_encrypted == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Malformed 2FAS backup: missing encrypted services.");
        g_free (twofas_data);
        json_decref (root);
        return NULL;
    }
    /* Build group UUID -> name map from outer (unencrypted) JSON */
    GHashTable *group_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    json_t *groups = json_object_get (root, "groups");
    if (groups != NULL && json_is_array (groups)) {
        for (guint gi = 0; gi < json_array_size (groups); gi++) {
            json_t *grp = json_array_get (groups, gi);
            const gchar *gid = json_string_value (json_object_get (grp, "id"));
            const gchar *gname = json_string_value (json_object_get (grp, "name"));
            if (gid != NULL && gname != NULL)
                g_hash_table_insert (group_map, g_strdup (gid), g_strdup (gname));
        }
    }

    gchar **b64_encoded_data = g_strsplit (services_encrypted, ":", 3);
    if (g_strv_length (b64_encoded_data) != 3) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Malformed 2FAS backup: 'servicesEncrypted' must be three colon-separated base64 fields.");
        g_strfreev (b64_encoded_data);
        g_hash_table_destroy (group_map);
        g_free (twofas_data);
        json_decref (root);
        return NULL;
    }
    if (!decrypt_data ((const gchar **)b64_encoded_data, password,
                       twofas_data, err)) {
        g_strfreev (b64_encoded_data);
        g_hash_table_destroy (group_map);
        g_free (twofas_data->salt);
        g_free (twofas_data->iv);
        g_free (twofas_data);
        json_decref (root);
        return NULL;
    }
    if (twofas_data->json_data != NULL) {
        otps = parse_twofas_json_data (twofas_data->json_data, group_map, err);
        gcry_free (twofas_data->json_data);
    }
    g_strfreev (b64_encoded_data);
    g_hash_table_destroy (group_map);
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
    if (!is_schema_supported (path, err)) {
        return NULL;
    }

    json_error_t j_err;
    json_t *json = json_load_file (path, JSON_DISABLE_EOF_CHECK | JSON_ALLOW_NUL, &j_err);
    if (!json) {
        g_printerr ("Error loading json: %s\n", j_err.text);
        return NULL;
    }

    /* Build group UUID -> name map */
    GHashTable *group_map = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    json_t *groups = json_object_get (json, "groups");
    if (groups != NULL && json_is_array (groups)) {
        for (guint gi = 0; gi < json_array_size (groups); gi++) {
            json_t *grp = json_array_get (groups, gi);
            const gchar *gid = json_string_value (json_object_get (grp, "id"));
            const gchar *gname = json_string_value (json_object_get (grp, "name"));
            if (gid != NULL && gname != NULL)
                g_hash_table_insert (group_map, g_strdup (gid), g_strdup (gname));
        }
    }

    json_t *services_obj = json_object_get (json, "services");
    if (services_obj == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Malformed 2FAS backup: missing 'services' array.");
        g_hash_table_destroy (group_map);
        json_decref (json);
        return NULL;
    }
    gchar *dumped_json = json_dumps (services_obj, 0);
    GSList *otps = parse_twofas_json_data (dumped_json, group_map, err);
    gcry_free (dumped_json);
    g_hash_table_destroy (group_map);
    json_decref (json);

    return otps;
}


static gboolean
is_schema_supported (const gchar *path,
                     GError     **err)
{
    json_t *root = get_json_root (path);
    if (root == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Could not parse the 2FAS backup JSON.");
        return FALSE;
    }
    gint32 schema_version = (gint32)json_integer_value (json_object_get (root, "schemaVersion"));
    if (schema_version != 4) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Unsupported 2FAS schema version: %d.", schema_version);
        json_decref (root);
        return FALSE;
    }
    json_decref (root);
    return TRUE;
}


static gboolean
decrypt_data (const gchar **b64_data,
              const gchar *pwd,
              TwofasData   *twofas_data,
              GError      **err)
{
    gsize enc_data_with_tag_size, salt_out_len, iv_out_len;
    guchar *enc_data_with_tag = g_base64_decode (b64_data[0], &enc_data_with_tag_size);
    twofas_data->salt = g_base64_decode (b64_data[1], &salt_out_len);
    twofas_data->iv = g_base64_decode (b64_data[2], &iv_out_len);

    if (enc_data_with_tag_size <= TWOFAS_TAG) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Encrypted 2FAS data is too small.");
        g_free (enc_data_with_tag);
        g_free (twofas_data->salt);
        g_free (twofas_data->iv);
        twofas_data->salt = NULL;
        twofas_data->iv = NULL;
        return FALSE;
    }
    if (salt_out_len != TWOFAS_SALT || iv_out_len != TWOFAS_IV) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Invalid salt or IV length in encrypted 2FAS backup.");
        g_free (enc_data_with_tag);
        g_free (twofas_data->salt);
        g_free (twofas_data->iv);
        twofas_data->salt = NULL;
        twofas_data->iv = NULL;
        return FALSE;
    }
    guchar tag[TWOFAS_TAG];
    gsize enc_buf_size = enc_data_with_tag_size - TWOFAS_TAG;
    guchar *enc_data = g_malloc0 (enc_buf_size);
    memcpy (enc_data, enc_data_with_tag, enc_buf_size);
    memcpy (tag, enc_data_with_tag+enc_buf_size, TWOFAS_TAG);
    g_free (enc_data_with_tag);

    guchar *derived_key = gcry_malloc_secure (32);
    if (derived_key == NULL) {
        g_set_error (err, secmem_alloc_error_gquark (), SECMEM_ALLOC_ERRCODE,
                     "Could not allocate secure memory for the 2FAS key.");
        g_free (enc_data);
        return FALSE;
    }
    // gcry_kdf_derive expects the password length in BYTES, not Unicode characters.
    gpg_error_t g_err = gcry_kdf_derive (pwd, strlen (pwd), GCRY_KDF_PBKDF2, GCRY_MD_SHA256,
                                         twofas_data->salt, salt_out_len, TWOFAS_KDF_ITERS, 32, derived_key);
    if (g_err != GPG_ERR_NO_ERROR) {
        g_set_error (err, key_deriv_gquark (), KEY_DERIVATION_ERRCODE,
                     "Failed to derive the 2FAS key: %s/%s",
                     gcry_strsource (g_err), gcry_strerror (g_err));
        explicit_bzero (derived_key, 32);
        gcry_free (derived_key);
        g_free (enc_data);
        return FALSE;
    }

    gcry_cipher_hd_t hd = open_cipher_and_set_data (derived_key, twofas_data->iv, iv_out_len);
    if (hd == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Failed to initialize the 2FAS cipher.");
        explicit_bzero (derived_key, 32);
        gcry_free (derived_key);
        g_free (enc_data);
        return FALSE;
    }

    twofas_data->json_data = gcry_calloc_secure (enc_buf_size + 1, 1);
    if (twofas_data->json_data == NULL) {
        g_set_error (err, secmem_alloc_error_gquark (), SECMEM_ALLOC_ERRCODE,
                     "Could not allocate secure memory for decrypted 2FAS data.");
        explicit_bzero (derived_key, 32);
        gcry_free (derived_key);
        g_free (enc_data);
        gcry_cipher_close (hd);
        return FALSE;
    }
    gpg_error_t gpg_err = gcry_cipher_decrypt (hd, twofas_data->json_data, enc_buf_size, enc_data, enc_buf_size);
    if (gpg_err) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Failed to decrypt 2FAS data: %s/%s",
                     gcry_strsource (gpg_err), gcry_strerror (gpg_err));
        gcry_free (twofas_data->json_data);
        twofas_data->json_data = NULL;
        explicit_bzero (derived_key, 32);
        gcry_free (derived_key);
        g_free (enc_data);
        gcry_cipher_close (hd);
        return FALSE;
    }

    gpg_err = gcry_cipher_checktag (hd, tag, TWOFAS_TAG);
    if (gpg_err) {
        g_set_error (err, bad_tag_gquark (), BAD_TAG_ERRCODE,
                     "Wrong password or corrupted 2FAS backup.");
        gcry_free (twofas_data->json_data);
        twofas_data->json_data = NULL;
    }

    gcry_cipher_close (hd);
    explicit_bzero (derived_key, 32);
    gcry_free (derived_key);
    g_free (enc_data);
    explicit_bzero (tag, sizeof (tag));
    return twofas_data->json_data != NULL;
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
        g_free (iv);
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
                        GHashTable  *group_map,
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
        if (otp_obj == NULL) {
            g_printerr ("Skipping malformed 2FAS entry (missing 'otp' object)\n");
            g_free (otp);
            continue;
        }
        otp->issuer = g_strdup (json_string_value (json_object_get (otp_obj, "issuer")));
        otp->account_name = g_strdup (json_string_value (json_object_get (otp_obj, "account")));
        otp->digits = (guint32) json_integer_value (json_object_get (otp_obj, "digits"));

        gboolean skip = FALSE;
        const gchar *type = json_string_value (json_object_get (otp_obj, "tokenType"));
        if (type == NULL) {
            g_printerr ("Skipping token due to missing type field\n");
            skip = TRUE;
        } else if (g_ascii_strcasecmp (type, "TOTP") == 0) {
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
        if (algo == NULL) {
            g_printerr ("Skipping token due to missing algorithm field\n");
            skip = TRUE;
        } else if (g_ascii_strcasecmp (algo, "SHA1") == 0 ||
                   g_ascii_strcasecmp (algo, "SHA256") == 0 ||
                   g_ascii_strcasecmp (algo, "SHA512") == 0) {
            otp->algo = g_utf8_strup (algo, -1);
        } else {
            g_printerr ("Skipping token due to unsupported algo: %s\n", algo);
            skip = TRUE;
        }

        if (!skip) {
            const gchar *group_id = json_string_value (json_object_get (obj, "groupId"));
            if (group_id != NULL && group_map != NULL) {
                const gchar *group_name = g_hash_table_lookup (group_map, group_id);
                otp->group = (group_name != NULL) ? g_strdup (group_name) : NULL;
            } else {
                otp->group = NULL;
            }
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
