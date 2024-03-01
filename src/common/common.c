#include <glib.h>
#include <gio/gio.h>
#include <sys/resource.h>
#include <cotp.h>
#include <glib/gi18n.h>
#include "gcrypt.h"
#include "jansson.h"
#include "common.h"
#include "../google-migration.pb-c.h"
#include "../file-size.h"
#include "../gquarks.h"

gint32
get_max_file_size_from_memlock (void)
{
    const gchar *link = "https://github.com/paolostivanin/OTPClient/wiki/Secure-Memory-Limitations";
    struct rlimit r;
    if (getrlimit (RLIMIT_MEMLOCK, &r) == -1) {
        // couldn't get memlock limit, so falling back to a default, low value
        g_print ("[WARNING] your operating system's memlock limit may be too low for you. Please have a look at %s\n", link);
        return LOW_MEMLOCK_VALUE;
    } else {
        if (r.rlim_cur == -1 || r.rlim_cur > MEMLOCK_VALUE) {
            // memlock is either unlimited or bigger than needed, so defaulting to 'MEMLOCK_VALUE'
            return MEMLOCK_VALUE;
        } else {
            // memlock is less than 'MEMLOCK_VALUE'
            g_print ("[WARNING] your operating system's memlock limit may be too low for you (current value: %d bytes).\n"
                     "This may cause issues when importing third parties databases or dealing with tens of tokens.\n"
                     "For information on how to increase the memlock value, please have a look at %s\n", (gint32)r.rlim_cur, link);
            return (gint32)r.rlim_cur;
        }
    }
}


gchar *
init_libs (gint32 max_file_size)
{
    gcry_control(GCRYCTL_SET_PREFERRED_RNG_TYPE, GCRY_RNG_TYPE_SYSTEM);
    if (!gcry_check_version ("1.8.0")) {
        return g_strdup ("The required version of GCrypt is 1.8.0 or greater.");
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
        algo_int = SHA1;
    } else if (g_strcmp0 (algo, "SHA256") == 0) {
        algo_int = SHA256;
    } else {
        algo_int = SHA512;
    }

    return algo_int;
}


guint32
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
    gchar *tmp_string = gcry_calloc_secure (256, 1);
    json_object_foreach (obj, key, value) {
        if (g_strcmp0 (key, "period") == 0 || g_strcmp0 (key, "counter") == 0 || g_strcmp0 (key, "digits") == 0) {
            json_int_t v = json_integer_value (value);
            g_snprintf (tmp_string + g_utf8_strlen (tmp_string, -1), 256, "%ld", (gint64) v);
        } else {
            if (g_strlcat (tmp_string, json_string_value (value), 256) > 256) {
                g_printerr ("%s\n", _("Truncation occurred."));
            }
        }
    }

    guint32 hash = jenkins_one_at_a_time_hash (tmp_string, strlen (tmp_string) + 1);

    gcry_free (tmp_string);

    return hash;
}

gchar *
secure_strdup (const gchar *src)
{
    gchar *sec_buf = gcry_calloc_secure (strlen (src) + 1, 1);
    memcpy (sec_buf, src, strlen (src) + 1);

    return sec_buf;
}


gchar *
g_trim_whitespace (const gchar *str)
{
    if (g_utf8_strlen (str, -1) == 0) {
        return NULL;
    }
    gchar *sec_buf = gcry_calloc_secure (strlen (str) + 1, 1);
    int pos = 0;
    for (int i = 0; str[i]; i++) {
        if (str[i] != ' ') {
            sec_buf[pos++] = str[i];
        }
    }
    sec_buf[pos] = '\0';
    gchar *secubf_newpos = (gchar *)gcry_realloc (sec_buf, strlen (sec_buf) + 1);

    return secubf_newpos;
}


guchar *
hexstr_to_bytes (const gchar *hexstr)
{
    size_t len = g_utf8_strlen (hexstr, -1);
    size_t final_len = len / 2;
    guchar *chrs = (guchar *)g_malloc ((final_len+1) * sizeof(*chrs));
    for (size_t i = 0, j = 0; j < final_len; i += 2, j++)
        chrs[j] = (hexstr[i] % 32 + 9) % 25 * 16 + (hexstr[i+1] % 32 + 9) % 25;
    chrs[final_len] = '\0';
    return chrs;
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


// Backported from Glib (needed by below function)
static int
unescape_character (const char *scanner)
{
    int first_digit;
    int second_digit;

    first_digit = g_ascii_xdigit_value (*scanner++);
    if (first_digit < 0)
        return -1;

    second_digit = g_ascii_xdigit_value (*scanner++);
    if (second_digit < 0)
        return -1;

    return (first_digit << 4) | second_digit;
}


// Backported from Glib. The only difference is that it's using gcrypt to allocate a secure buffer.
gchar *
g_uri_unescape_string_secure (const gchar *escaped_string,
                              const gchar *illegal_characters)
{
    if (escaped_string == NULL)
        return NULL;

    const gchar *escaped_string_end = escaped_string + g_utf8_strlen (escaped_string, -1);

    gchar *result = gcry_calloc_secure (escaped_string_end - escaped_string + 1, 1);
    gchar *out = result;

    const gchar *in;
    gint character;
    for (in = escaped_string; in < escaped_string_end; in++) {
        character = *in;

        if (*in == '%') {
            in++;
            if (escaped_string_end - in < 2) {
                // Invalid escaped char (to short)
                gcry_free (result);
                return NULL;
            }

            character = unescape_character (in);

            // Check for an illegal character. We consider '\0' illegal here.
            if (character <= 0 ||
                (illegal_characters != NULL &&
                 strchr (illegal_characters, (char)character) != NULL)) {
                gcry_free (result);
                return NULL;
            }

            in++; // The other char will be eaten in the loop header
        }
        *out++ = (char)character;
    }

    *out = '\0';

    return result;
}


guchar *
g_base64_decode_secure (const gchar *text,
                        gsize       *out_len)
{
    guchar *ret;
    gsize input_length;
    gint state = 0;
    guint save = 0;

    g_return_val_if_fail (text != NULL, NULL);
    g_return_val_if_fail (out_len != NULL, NULL);

    input_length = g_utf8_strlen (text, -1);

    /* We can use a smaller limit here, since we know the saved state is 0,
       +1 used to avoid calling g_malloc0(0), and hence returning NULL */
    ret = gcry_calloc_secure ((input_length / 4) * 3 + 1, 1);

    *out_len = g_base64_decode_step (text, input_length, ret, &state, &save);

    return ret;
}


GSList *
decode_migration_data (const gchar *encoded_uri)
{
    const gchar *encoded_uri_copy = encoded_uri;
    if (g_ascii_strncasecmp (encoded_uri_copy, "otpauth-migration://offline?data=", 33) != 0) {
        return NULL;
    }
    encoded_uri_copy += 33;
    gsize out_len;
    gchar *unesc_str = g_uri_unescape_string_secure (encoded_uri_copy, NULL);
    guchar *data = g_base64_decode_secure (unesc_str, &out_len);
    gcry_free (unesc_str);

    GSList *uris = NULL;
    GString *uri = NULL;
    MigrationPayload *msg = migration_payload__unpack (NULL, out_len, data);
    gcry_free (data);
    for (gint i = 0; i < msg->n_otp_parameters; i++) {
        uri = g_string_new ("otpauth://");
        if (msg->otp_parameters[i]->type == 1) {
            g_string_append (uri, "hotp/");
        } else if (msg->otp_parameters[i]->type == 2) {
            g_string_append (uri, "totp/");
        } else {
            g_printerr ("OTP type not recognized, skipping %s\n", msg->otp_parameters[i]->name);
            goto end;
        }

        g_string_append (uri, msg->otp_parameters[i]->name);
        g_string_append (uri, "?");

        if (msg->otp_parameters[i]->algorithm == 1) {
            g_string_append (uri, "algorithm=SHA1&");
        } else if (msg->otp_parameters[i]->algorithm == 2) {
            g_string_append (uri, "algorithm=SHA256&");
        } else if (msg->otp_parameters[i]->algorithm == 3) {
            g_string_append (uri, "algorithm=SHA512&");
        } else {
            g_printerr ("Algorithm type not supported, skipping %s\n", msg->otp_parameters[i]->name);
            goto end;
        }

        if (msg->otp_parameters[i]->digits == 1) {
            g_string_append (uri, "digits=6&");
        } else if (msg->otp_parameters[i]->digits == 2) {
            g_string_append (uri, "digits=8&");
        } else {
            g_printerr ("Algorithm type not supported, skipping %s\n", msg->otp_parameters[i]->name);
            goto end;
        }

        if (msg->otp_parameters[i]->issuer != NULL) {
            g_string_append (uri, "issuer=");
            g_string_append (uri, msg->otp_parameters[i]->issuer);
            g_string_append (uri, "&");
        }

        if (msg->otp_parameters[i]->type == 1) {
            g_string_append (uri, "counter=");
            g_string_append_printf(uri, "%ld", msg->otp_parameters[i]->counter);
            g_string_append (uri, "&");
        }

#ifdef COTP_OLD_LIB
        baseencode_error_t b_err;
#else
        cotp_error_t b_err;
#endif
        gchar *b32_encoded_secret = base32_encode (msg->otp_parameters[i]->secret.data, msg->otp_parameters[i]->secret.len, &b_err);
        if (b32_encoded_secret == NULL) {
            g_printerr ("Error while encoding the secret (error code %d)\n", b_err);
            goto end;
        }

        g_string_append (uri, "secret=");
        g_string_append (uri, b32_encoded_secret);

        uris = g_slist_append (uris, g_strdup (uri->str));

        end:
        g_string_free (uri, TRUE);
    }

    migration_payload__free_unpacked (msg, NULL);

    return uris;
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


GKeyFile *
get_kf_ptr (void)
{
    GError *err = NULL;
    GKeyFile *kf = g_key_file_new ();
    gchar *cfg_file_path;
#ifndef USE_FLATPAK_APP_FOLDER
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


guchar *
get_andotp_derived_key (const gchar  *password,
                        const guchar *salt,
                        guint32       iterations)
{
    guchar *derived_key = gcry_malloc_secure (32);
    gpg_error_t g_err = gcry_kdf_derive (password, (gsize)g_utf8_strlen (password, -1), GCRY_KDF_PBKDF2, GCRY_MD_SHA1,
                                         salt, ANDOTP_IV_SALT, iterations, 32, derived_key);
    if (g_err != GPG_ERR_NO_ERROR) {
        g_printerr ("Failed to derive key: %s/%s\n", gcry_strsource (g_err), gcry_strerror (g_err));
        gcry_free (derived_key);
        return NULL;
    }

    return derived_key;
}


gchar *
get_data_from_encrypted_backup (const gchar       *path,
                                const gchar       *password,
                                gint32             max_file_size,
                                gint32             provider,
                                guint32            andotp_be_iterations,
                                GFile             *in_file,
                                GFileInputStream  *in_stream,
                                GError           **err)
{
    gint32 salt_size = 0, iv_size = 0, tag_size = 0;
    switch (provider) {
        case ANDOTP:
            salt_size = iv_size = ANDOTP_IV_SALT;
            tag_size = ANDOTP_TAG;
            break;
        case AUTHPRO:
            salt_size = tag_size = AUTHPRO_SALT_TAG;
            iv_size = AUTHPRO_IV;
            break;
    }

    guchar salt[salt_size];
    if (g_input_stream_read (G_INPUT_STREAM (in_stream), salt, salt_size, NULL, err) == -1) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    }

    guchar iv[iv_size];
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
    guchar tag[tag_size];
    if (g_input_stream_read (G_INPUT_STREAM (in_stream), tag, tag_size, NULL, err) == -1) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    }

    gsize enc_buf_size;
    gint32 offset = 0;
    switch (provider) {
        case ANDOTP:
            // 4 is the size of iterations (int32)
            offset = 4;
            break;
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
        g_set_error (err, file_too_big_gquark (), FILE_TOO_BIG, "File is too big");
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
        case ANDOTP:
            derived_key = get_andotp_derived_key (password, salt, andotp_be_iterations);
            break;
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