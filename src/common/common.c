#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <sys/resource.h>
#include <cotp.h>
#include "gcrypt.h"
#include "jansson.h"
#include "common.h"
#include "file-size.h"
#include "gquarks.h"


gint32
set_memlock_value (gint32 *memlock_value)
{
    struct rlimit r;
    if (getrlimit (RLIMIT_MEMLOCK, &r) == -1) {
        // if memlock cannot be retrieved, return an error
        return MEMLOCK_ERR;
    }

    if (r.rlim_cur < DEFAULT_MEMLOCK_VALUE) {
        // memlock is less than the default value, so we need to warn the user that there might not be enough secmem available.
        *memlock_value = (gint32) r.rlim_cur;
        return MEMLOCK_TOO_LOW;
    }

    *memlock_value = DEFAULT_MEMLOCK_VALUE;
    return MEMLOCK_OK;
}


gchar *
init_libs (gint32 max_file_size)
{
    gcry_control(GCRYCTL_SET_PREFERRED_RNG_TYPE, GCRY_RNG_TYPE_SYSTEM);
    if (!gcry_check_version ("1.10.1")) {
        return g_strdup ("The required version of GCrypt is 1.10.1 or greater.");
    }

    if (gcry_control (GCRYCTL_INIT_SECMEM, max_file_size, 0)) {
        return g_strdup ("Couldn't initialize secure memory.\n");
    }
    gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);

    json_set_alloc_funcs (gcry_malloc_secure, gcry_free);

    return NULL;
}


gint
get_algo_int_from_str (const gchar *algo)
{
    gint algo_int;
    if (g_strcmp0 (algo, "SHA1") == 0) {
        algo_int = COTP_SHA1;
    } else if (g_strcmp0 (algo, "SHA256") == 0) {
        algo_int = COTP_SHA256;
    } else {
        algo_int = COTP_SHA512;
    }

    return algo_int;
}


gchar *
secure_strdup (const gchar *src)
{
    if (src == NULL) {
        return NULL;
    }
    size_t len = strlen (src);
    gchar *sec_buf = gcry_calloc_secure (len + 1, 1);
    if (sec_buf == NULL) {
        return NULL;
    }
    memcpy (sec_buf, src, len + 1);
    return sec_buf;
}


guchar *
hexstr_to_bytes (const gchar *hexstr)
{
    if (hexstr == NULL) {
        return NULL;
    }
    // Hex strings should be ASCII; use strlen, not g_utf8_strlen
    size_t len = strlen (hexstr);
    if (len == 0 || (len % 2) != 0) {
        // invalid length
        return NULL;
    }

    size_t out_len = len / 2;
    guchar *bytes = (guchar *) g_malloc (out_len + 1); // +1 to NUL-terminate if needed by callers
    if (bytes == NULL) {
        return NULL;
    }

    for (size_t i = 0, j = 0; j < out_len; i += 2, j++) {
        int hi = g_ascii_xdigit_value (hexstr[i]);
        int lo = g_ascii_xdigit_value (hexstr[i + 1]);
        if (hi < 0 || lo < 0) {
            g_free (bytes);
            return NULL;
        }
        bytes[j] = (guchar) ((hi << 4) | lo);
    }
    bytes[out_len] = '\0';
    return bytes;
}


gchar *
bytes_to_hexstr (const guchar *data, size_t datalen)
{
    gchar hex_str[]= "0123456789abcdef";

    gchar *result = g_malloc0(datalen * 2 + 1);
    if (result == NULL) {
        g_printerr ("Error while allocating memory for bytes_to_hexstr.\n");
        return result;
    }

    for (guint i = 0; i < datalen; i++)
    {
        result[i * 2 + 0] = hex_str[(data[i] >> 4) & 0x0F];
        result[i * 2 + 1] = hex_str[(data[i]     ) & 0x0F];
    }

    result[datalen * 2] = 0;

    return result;
}


gcry_cipher_hd_t
open_cipher_and_set_data (guchar *derived_key,
                          guchar *iv,
                          gsize   iv_len)
{
    gcry_cipher_hd_t hd;
    gpg_error_t gpg_err = gcry_cipher_open (&hd, GCRY_CIPHER_AES256, GCRY_CIPHER_MODE_GCM, GCRY_CIPHER_SECURE);
    if (gpg_err) {
        g_printerr ("%s\n", _("Error while opening the cipher handle."));
        return NULL;
    }

    gpg_err = gcry_cipher_setkey (hd, derived_key, gcry_cipher_get_algo_keylen (GCRY_CIPHER_AES256));
    if (gpg_err) {
        g_printerr ("%s\n", _("Error while setting the cipher key."));
        gcry_cipher_close (hd);
        return NULL;
    }

    gpg_err = gcry_cipher_setiv (hd, iv, iv_len);
    if (gpg_err) {
        g_printerr ("%s\n", _("Error while setting the cipher iv."));
        gcry_cipher_close (hd);
        return NULL;
    }

    return hd;
}


guchar *
get_authpro_derived_key (const gchar *password,
                         const guchar *salt)
{
    guchar *derived_key = gcry_malloc_secure (32);
    // taglen, iterations, memory_cost (65536=64MiB), parallelism
    const unsigned long params[4] = {32, 3, 65536, 4};
    gcry_kdf_hd_t hd;
    if (gcry_kdf_open (&hd, GCRY_KDF_ARGON2, GCRY_KDF_ARGON2ID,
                       params, 4,
                       password,  (gsize)g_utf8_strlen (password, -1),
                       salt, AUTHPRO_SALT_TAG,
                       NULL, 0, NULL, 0) != GPG_ERR_NO_ERROR) {
        g_printerr ("Error while opening the KDF handler\n");
        gcry_free (derived_key);
        return NULL;
    }
    if (gcry_kdf_compute (hd, NULL) != GPG_ERR_NO_ERROR) {
        g_printerr ("Error while computing the KDF\n");
        gcry_free (derived_key);
        gcry_kdf_close (hd);
        return NULL;
    }
    if (gcry_kdf_final (hd, 32, derived_key) != GPG_ERR_NO_ERROR) {
        g_printerr ("Error while finalizing the KDF handler\n");
        gcry_free (derived_key);
        gcry_kdf_close (hd);
        return NULL;
    }

    gcry_kdf_close (hd);

    return derived_key;
}


gchar *
get_data_from_encrypted_backup (const gchar       *path,
                                const gchar       *password,
                                gint32             max_file_size,
                                gint32             provider,
                                GFile             *in_file,
                                GFileInputStream  *in_stream,
                                GError           **err)
{
    gint32 salt_size = 0, iv_size = 0, tag_size = 0;
    switch (provider) {
        case AUTHPRO:
            salt_size = tag_size = AUTHPRO_SALT_TAG;
            iv_size = AUTHPRO_IV;
            break;
    }

    g_autofree guchar *salt = g_malloc0 (salt_size);
    if (g_input_stream_read (G_INPUT_STREAM (in_stream), salt, salt_size, NULL, err) == -1) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    }

    g_autofree guchar *iv = g_malloc0 (iv_size);
    if (g_input_stream_read (G_INPUT_STREAM (in_stream), iv, iv_size, NULL, err) == -1) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    }

    goffset input_file_size = get_file_size (path);
    if (!g_seekable_seek (G_SEEKABLE (in_stream), input_file_size - tag_size, G_SEEK_SET, NULL, err)) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    }
    g_autofree guchar *tag = g_malloc0 (tag_size);
    if (g_input_stream_read (G_INPUT_STREAM (in_stream), tag, tag_size, NULL, err) == -1) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    }

    gsize enc_buf_size;
    gint32 offset = 0;
    switch (provider) {
        case AUTHPRO:
            // 16 is the size of the header
            offset = 16;
            break;
    }
    enc_buf_size = (gsize)(input_file_size - offset - salt_size - iv_size - tag_size);
    if (enc_buf_size < 1) {
        g_printerr ("A non-encrypted file has been selected\n");
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    } else if (enc_buf_size > max_file_size) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        g_set_error (err, file_too_big_gquark (), FILE_TOO_BIG_ERRCODE, FILE_SIZE_SECMEM_MSG);
        return NULL;
    }

    guchar *enc_buf = g_malloc0 (enc_buf_size);
    if (!g_seekable_seek (G_SEEKABLE(in_stream), offset + salt_size + iv_size, G_SEEK_SET, NULL, err)) {
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

    guchar *derived_key = NULL;
    switch (provider) {
        case AUTHPRO:
            derived_key = get_authpro_derived_key (password, salt);
            break;
    }

    if (derived_key == NULL) {
        g_free (enc_buf);
        return NULL;
    }

    gcry_cipher_hd_t hd = open_cipher_and_set_data (derived_key, iv, iv_size);
    if (hd == NULL) {
        gcry_free (derived_key);
        g_free (enc_buf);
        return NULL;
    }

    gchar *decrypted_data = gcry_calloc_secure (enc_buf_size, 1);
    gpg_error_t gpg_err = gcry_cipher_decrypt (hd, decrypted_data, enc_buf_size, enc_buf, enc_buf_size);
    if (gpg_err) {
        g_free (enc_buf);
        gcry_free (derived_key);
        gcry_free (decrypted_data);
        gcry_cipher_close (hd);
        return NULL;
    }
    if (gcry_err_code (gcry_cipher_checktag (hd, tag, tag_size)) == GPG_ERR_CHECKSUM) {
        g_set_error (err, bad_tag_gquark (), BAD_TAG_ERRCODE, "Either the file is corrupted or the password is wrong");
        gcry_cipher_close (hd);
        g_free (enc_buf);
        gcry_free (derived_key);
        gcry_free (decrypted_data);
        return NULL;
    }

    gcry_cipher_close (hd);
    gcry_free (derived_key);
    g_free (enc_buf);

    return decrypted_data;
}


static guint32
jenkins_one_at_a_time_hash (const gchar *key, gsize len)
{
    guint32 hash, i;
    for (hash = i = 0; i < len; ++i) {
        hash += key[i];
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);

    return hash;
}


guint32
json_object_get_hash (json_t *obj)
{
    const gchar *key;
    json_t *value;
    const gsize buf_size = 256;
    gchar *tmp_string = gcry_calloc_secure (buf_size, 1);
    json_object_foreach (obj, key, value) {
        if (g_strcmp0 (key, "period") == 0 || g_strcmp0 (key, "counter") == 0 || g_strcmp0 (key, "digits") == 0) {
            json_int_t v = json_integer_value (value);
            gsize cur_len = strlen (tmp_string);
            if (cur_len < buf_size - 1) {
                g_snprintf (tmp_string + cur_len, buf_size - cur_len, "%" G_GINT64_FORMAT, (gint64) v);
            }
        } else {
            const gchar *str_val = json_string_value (value);
            if (str_val != NULL) {
                if (g_strlcat (tmp_string, str_val, buf_size) >= buf_size) {
                    g_printerr ("%s\n", _("Truncation occurred."));
                }
            }
        }
    }

    guint32 hash = jenkins_one_at_a_time_hash (tmp_string, strlen (tmp_string) + 1);

    gcry_free (tmp_string);

    return hash;
}


void
free_otps_gslist (GSList *otps,
                  guint   list_len)
{
    otp_t *otp_data;
    for (guint i = 0; i < list_len; i++) {
        otp_data = g_slist_nth_data (otps, i);
        g_free (otp_data->type);
        g_free (otp_data->algo);
        g_free (otp_data->account_name);
        g_free (otp_data->issuer);
        gcry_free (otp_data->secret);
    }
    g_slist_free (otps);
}


json_t *
build_json_obj (const gchar *type,
                const gchar *acc_label,
                const gchar *acc_iss,
                const gchar *acc_key,
                guint        digits,
                const gchar *algo,
                guint        period,
                guint64      ctr)
{
    json_t *obj = json_object ();
    json_object_set (obj, "type", json_string (type));
    json_object_set (obj, "label", json_string (acc_label));
    json_object_set (obj, "issuer", json_string (acc_iss));
    json_object_set (obj, "secret", json_string (acc_key));
    json_object_set (obj, "digits", json_integer (digits));
    json_object_set (obj, "algo", json_string (algo));

    if (g_ascii_strcasecmp (type, "TOTP") == 0) {
        json_object_set (obj, "period", json_integer (period));
    } else {
        json_object_set (obj, "counter", json_integer ((json_int_t)ctr));
    }

    return obj;
}


json_t *
get_json_root (const gchar *path)
{
    json_error_t jerr;
    json_t *json = json_load_file (path, JSON_DISABLE_EOF_CHECK | JSON_ALLOW_NUL, &jerr);
    if (!json) {
        g_printerr ("Error loading the json file: %s\n", jerr.text);
        return NULL;
    }

    gchar *dumped_json = json_dumps (json, 0);
    json_decref (json);

    json_t *root = json_loads (dumped_json, JSON_DISABLE_EOF_CHECK, &jerr);
    if (root == NULL) {
        g_printerr ("Error while loading the json data: %s\n", jerr.text);
        gcry_free (dumped_json);
        return NULL;
    }
    gcry_free (dumped_json);

    return root;
}


void
json_free (gpointer data)
{
    json_decref (data);
}


GKeyFile *
get_kf_ptr (void)
{
    GError *err = NULL;
    GKeyFile *kf = g_key_file_new ();
    gchar *cfg_file_path;
#ifndef IS_FLATPAK
    cfg_file_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
#else
    cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#endif
    if (g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
        if (g_key_file_load_from_file (kf, cfg_file_path, G_KEY_FILE_NONE, &err)) {
            g_free (cfg_file_path);
            return kf;
        }
        g_printerr ("%s\n", err->message);
        g_clear_error (&err);
    }
    g_free (cfg_file_path);
    g_key_file_free (kf);
    return NULL;
}


gboolean
is_secmem_available (gsize    required_size,
                     GError **err)
{
    void *test = gcry_malloc_secure (required_size);
    if (test == NULL) {
        g_set_error(err, secmem_alloc_error_gquark (), NO_SECMEM_AVAIL_ERRCODE,
                   "Not enough secure memory available (%zu bytes requested).",
                   required_size);
        return FALSE;
    }
    gcry_free (test);
    return TRUE;
}