#include <glib.h>
#include <gio/gio.h>
#include <jansson.h>
#include <gcrypt.h>
#include "common.h"
#include "../gquarks.h"
#include "../imports.h"

#define KDF_ITERS 10000

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

static GSList   *parse_twofas_json_data         (const gchar       *data,
                                                 GError           **err);


GSList *
get_twofas_data (const gchar  *path,
                 const gchar  *password,
                 GError      **err)
{
    return (password != NULL) ? get_otps_from_encrypted_backup (path, password, err) : get_otps_from_plain_backup (path, err);
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
    gsize data_out_len, salt_out_len, iv_out_len;
    guchar *enc_data = g_base64_decode (b64_data[0], &data_out_len);
    twofas_data->salt = g_base64_decode (b64_data[1], &salt_out_len);
    twofas_data->iv = g_base64_decode (b64_data[2], &iv_out_len);

    guchar *derived_key = gcry_malloc_secure (32);
    gpg_error_t g_err = gcry_kdf_derive (pwd, (gsize)g_utf8_strlen (pwd, -1), GCRY_KDF_PBKDF2, GCRY_MD_SHA256,
                                         twofas_data->salt, salt_out_len, KDF_ITERS, 32, derived_key);
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