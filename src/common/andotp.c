#include <glib.h>
#include <gio/gio.h>
#include <gcrypt.h>
#include <jansson.h>
#include <time.h>
#include <glib/gi18n.h>
#include "../file-size.h"
#include "../imports.h"
#include "../gquarks.h"
#include "common.h"

#define ANDOTP_IV_SIZE   12
#define ANDOTP_SALT_SIZE 12
#define ANDOTP_TAG_SIZE  16
#define PBKDF2_MIN_BACKUP_ITERATIONS 140000
#define PBKDF2_MAX_BACKUP_ITERATIONS 160000

static GSList *get_otps_from_encrypted_backup (const gchar          *path,
                                               const gchar          *password,
                                               gint32                max_file_size,
                                               GFile                *in_file,
                                               GFileInputStream     *in_stream,
                                               GError              **err);

static GSList *get_otps_from_plain_backup     (const gchar          *path,
                                               GError              **err);

static guchar *get_derived_key                (const gchar          *password,
                                               const guchar         *salt,
                                               guint32               iterations);

static GSList *parse_json_data                (const gchar          *data,
                                               GError              **err);


GSList *
get_andotp_data (const gchar     *path,
                 const gchar     *password,
                 gint32           max_file_size,
                 gboolean         encrypted,
                 GError         **err)
{
    GFile *in_file = g_file_new_for_path(path);
    GFileInputStream *in_stream = g_file_read(in_file, NULL, err);
    if (*err != NULL) {
        g_object_unref(in_file);
        return NULL;
    }

    return (encrypted == TRUE) ? get_otps_from_encrypted_backup (path, password, max_file_size, in_file, in_stream, err) : get_otps_from_plain_backup (path, err);
}


static GSList *
get_otps_from_encrypted_backup (const gchar          *path,
                                const gchar          *password,
                                gint32                max_file_size,
                                GFile                *in_file,
                                GFileInputStream     *in_stream,
                                GError              **err)
{
    gint32 le_iterations;
    if (g_input_stream_read (G_INPUT_STREAM (in_stream), &le_iterations, 4, NULL, err) == -1) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    }

    guint32 be_iterations = __builtin_bswap32 (le_iterations);
    if (be_iterations < PBKDF2_MIN_BACKUP_ITERATIONS || be_iterations > PBKDF2_MAX_BACKUP_ITERATIONS) {
        // https://github.com/andOTP/andOTP/blob/6c54b8811f950375c774b2eefebcf1f9fa13d433/app/src/main/java/org/shadowice/flocke/andotp/Utilities/Constants.java#L124-L125
        g_object_unref (in_stream);
        g_object_unref (in_file);
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Number of iterations is invalid. It's likely this is not an andOTP encrypted database.\n");
        return NULL;
    }

    guchar salt[ANDOTP_SALT_SIZE];
    if (g_input_stream_read (G_INPUT_STREAM (in_stream), salt, ANDOTP_SALT_SIZE, NULL, err) == -1) {
        g_object_unref (in_stream);
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
    guchar tag[ANDOTP_TAG_SIZE];
    if (!g_seekable_seek (G_SEEKABLE (in_stream), input_file_size - ANDOTP_TAG_SIZE, G_SEEK_SET, NULL, err)) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    }
    if (g_input_stream_read (G_INPUT_STREAM (in_stream), tag, ANDOTP_TAG_SIZE, NULL, err) == -1) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    }

    // 4 is the size of iterations (int32)
    gsize enc_buf_size = (gsize) input_file_size - 4 - ANDOTP_SALT_SIZE - ANDOTP_IV_SIZE - ANDOTP_TAG_SIZE;
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
    if (!g_seekable_seek (G_SEEKABLE (in_stream), 4 + ANDOTP_SALT_SIZE + ANDOTP_IV_SIZE, G_SEEK_SET, NULL, err)) {
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

    guchar *derived_key = get_derived_key (password, salt, be_iterations);

    gcry_cipher_hd_t hd = open_cipher_and_set_data (derived_key, iv, ANDOTP_IV_SIZE);
    if (hd == NULL) {
        gcry_free (derived_key);
        g_free (enc_buf);
        return NULL;
    }

    gchar *decrypted_json = gcry_calloc_secure (enc_buf_size, 1);
    gpg_error_t gpg_err = gcry_cipher_decrypt (hd, decrypted_json, enc_buf_size, enc_buf, enc_buf_size);
    if (gpg_err) {
        g_free (enc_buf);
        gcry_free (derived_key);
        gcry_free (decrypted_json);
        gcry_cipher_close (hd);
        return NULL;
    }
    if (gcry_err_code (gcry_cipher_checktag (hd, tag, ANDOTP_TAG_SIZE)) == GPG_ERR_CHECKSUM) {
        g_set_error (err, bad_tag_gquark (), BAD_TAG_ERRCODE, "Either the file is corrupted or the password is wrong");
        gcry_cipher_close (hd);
        g_free (enc_buf);
        gcry_free (derived_key);
        gcry_free (decrypted_json);
        return NULL;
    }

    gcry_cipher_close (hd);
    gcry_free (derived_key);
    g_free (enc_buf);

    GSList *otps = parse_json_data (decrypted_json, err);
    gcry_free (decrypted_json);

    return otps;
}


static GSList *
get_otps_from_plain_backup (const gchar  *path,
                            GError      **err)
{
    gchar *plain_json_data;
    gsize read_len;
    if (!g_file_get_contents (path, &plain_json_data, &read_len, err)) {
        return NULL;
    }

    GSList *otps = parse_json_data (plain_json_data, err);
    g_free (plain_json_data);

    return otps;
}


gchar *
export_andotp (const gchar *export_path,
               const gchar *password,
               json_t *json_db_data)
{
    GError *err = NULL;
    json_t *array = json_array ();
    json_t *db_obj, *export_obj;
    gsize index;
    json_array_foreach (json_db_data, index, db_obj) {
        export_obj = json_object ();

        const gchar *issuer = json_string_value (json_object_get (db_obj, "issuer"));
        if (issuer != NULL && g_ascii_strcasecmp (issuer, "steam") == 0) {
            json_object_set (export_obj, "type", json_string ("STEAM"));
        } else {
            json_object_set (export_obj, "type", json_object_get (db_obj, "type"));
        }
        json_object_set(export_obj, "issuer", json_object_get (db_obj, "issuer"));
        json_object_set (export_obj, "label", json_object_get (db_obj, "label"));
        json_object_set (export_obj, "secret", json_object_get (db_obj, "secret"));
        json_object_set (export_obj, "digits", json_object_get (db_obj, "digits"));
        json_object_set (export_obj, "algorithm", json_object_get (db_obj, "algo"));
        if (g_ascii_strcasecmp (json_string_value (json_object_get (db_obj, "type")), "TOTP") == 0) {
            json_object_set (export_obj, "period", json_object_get (db_obj, "period"));
        } else {
            json_object_set (export_obj, "counter", json_object_get (db_obj, "counter"));
        }
        json_array_append (array, export_obj);
    }

    // if plaintext export is needed, then write the file and exit
    if (password == NULL) {
        GFile *out_gfile = g_file_new_for_path (export_path);
        GFileOutputStream *out_stream = g_file_replace (out_gfile, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION | G_FILE_CREATE_PRIVATE, NULL, &err);
        if (err == NULL) {
            gsize jbuf_size = json_dumpb (array, NULL, 0, 0);
            if (jbuf_size == 0) {
                g_object_unref (out_stream);
                g_object_unref (out_gfile);
                goto end;
            }
            gchar *jbuf = g_malloc0 (jbuf_size);
            if (json_dumpb (array, jbuf, jbuf_size, JSON_COMPACT) == -1) {
                g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "couldn't dump json data to buffer");
                g_free (jbuf);
                g_object_unref (out_stream);
                g_object_unref (out_gfile);
                goto end;
            }
            if (g_output_stream_write (G_OUTPUT_STREAM(out_stream), jbuf, jbuf_size, NULL, &err) == -1) {
                g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "couldn't dump json data to file");
            }
            g_free (jbuf);
            g_object_unref (out_stream);
            g_object_unref (out_gfile);
        } else {
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "couldn't create the file object");
            g_object_unref (out_gfile);
        }
        goto end;
    }

    gchar *json_data = json_dumps (array, JSON_COMPACT);
    if (json_data == NULL) {
        json_array_clear (array);
        g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "couldn't dump json data");
        goto end;
    }
    gsize json_data_size = strlen (json_data);

    // https://github.com/andOTP/andOTP/blob/bb01bbd242ace1a2e2620263d950d9852772f051/app/src/main/java/org/shadowice/flocke/andotp/Utilities/Constants.java#L109-L110
    guint32 le_iterations = (g_random_int () % (PBKDF2_MAX_BACKUP_ITERATIONS - PBKDF2_MIN_BACKUP_ITERATIONS + 1)) + PBKDF2_MIN_BACKUP_ITERATIONS;
    gint32 be_iterations = (gint32)__builtin_bswap32 (le_iterations);

    guchar *iv = g_malloc0 (ANDOTP_IV_SIZE);
    gcry_create_nonce (iv, ANDOTP_IV_SIZE);

    guchar *salt = g_malloc0 (ANDOTP_SALT_SIZE);
    gcry_create_nonce (salt, ANDOTP_SALT_SIZE);

    guchar *derived_key = get_derived_key (password, salt, le_iterations);
    gcry_cipher_hd_t hd = open_cipher_and_set_data (derived_key, iv, ANDOTP_IV_SIZE);
    if (hd == NULL) {
        gcry_free (derived_key);
        g_free (iv);
        g_free (salt);
        return NULL;
    }

    gchar *enc_buf = gcry_calloc_secure (json_data_size, 1);
    gpg_error_t gpg_err = gcry_cipher_encrypt (hd, enc_buf, json_data_size, json_data, json_data_size);
    if (gpg_err) {
        g_printerr ("%s\n", _("Error while encrypting the data."));
        gcry_free (derived_key);
        gcry_free (enc_buf);
        g_free (iv);
        g_free (salt);
        gcry_cipher_close (hd);
        return NULL;
    }
    guchar tag[ANDOTP_TAG_SIZE];
    gcry_cipher_gettag (hd, tag, ANDOTP_TAG_SIZE);
    gcry_cipher_close (hd);

    GFile *out_gfile = g_file_new_for_path (export_path);
    GFileOutputStream *out_stream = g_file_replace (out_gfile, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION | G_FILE_CREATE_PRIVATE, NULL, &err);
    if (err != NULL) {
        goto cleanup_before_exiting;
    }
    if (g_output_stream_write (G_OUTPUT_STREAM (out_stream), &be_iterations, 4, NULL, &err) == -1) {
        goto cleanup_before_exiting;
    }
    if (g_output_stream_write (G_OUTPUT_STREAM (out_stream), salt, ANDOTP_SALT_SIZE, NULL, &err) == -1) {
        goto cleanup_before_exiting;
    }
    if (g_output_stream_write (G_OUTPUT_STREAM (out_stream), iv, ANDOTP_IV_SIZE, NULL, &err) == -1) {
        goto cleanup_before_exiting;
    }
    if (g_output_stream_write (G_OUTPUT_STREAM (out_stream), enc_buf, json_data_size, NULL, &err) == -1) {
        goto cleanup_before_exiting;
    }
    if (g_output_stream_write (G_OUTPUT_STREAM (out_stream), tag, ANDOTP_TAG_SIZE, NULL, &err) == -1) {
        goto cleanup_before_exiting;
    }

    cleanup_before_exiting:
    g_free (iv);
    g_free (salt);
    gcry_free (json_data);
    gcry_free (derived_key);
    gcry_free (enc_buf);
    json_array_clear (array);
    g_object_unref (out_stream);
    g_object_unref (out_gfile);

    end:
    return (err != NULL ? g_strdup (err->message) : NULL);
}


static guchar *
get_derived_key (const gchar  *password,
                 const guchar *salt,
                 guint32       iterations)
{
    guchar *derived_key = gcry_malloc_secure (32);
    if (gcry_kdf_derive (password, (gsize) g_utf8_strlen (password, -1), GCRY_KDF_PBKDF2, GCRY_MD_SHA1,
                         salt, ANDOTP_SALT_SIZE, iterations, 32, derived_key) != 0) {
        gcry_free (derived_key);
        return NULL;
    }

    return derived_key;
}


static GSList *
parse_json_data (const gchar *data,
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

        const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
        if (issuer != NULL && g_utf8_strlen (issuer, -1) > 1) {
            otp->issuer = g_strstrip (g_strdup (issuer));
        }

        const gchar *label_with_prefix = json_string_value (json_object_get (obj, "label"));
        gchar **tokens = g_strsplit (label_with_prefix, ":", -1);
        if (tokens[0] && tokens[1]) {
            if (issuer != NULL && g_ascii_strcasecmp(issuer, tokens[0]) == 0) {
                otp->account_name = g_strstrip (g_strdup (tokens[1]));
            } else {
                otp->issuer = g_strstrip (g_strdup (tokens[0]));
                otp->account_name = g_strstrip (g_strdup (tokens[1]));
            }
        } else {
            otp->account_name = g_strstrip (g_strdup (tokens[0]));
        }
        g_strfreev (tokens);

        otp->digits = (guint32) json_integer_value (json_object_get(obj, "digits"));

        const gchar *type = json_string_value (json_object_get (obj, "type"));
        if (g_ascii_strcasecmp (type, "TOTP") == 0) {
            otp->type = g_strdup (type);
            otp->period = (guint32)json_integer_value (json_object_get (obj, "period"));
        } else if (g_ascii_strcasecmp (type, "HOTP") == 0) {
            otp->type = g_strdup (type);
            otp->counter = json_integer_value (json_object_get (obj, "counter"));
        } else if (g_ascii_strcasecmp (type, "Steam") == 0) {
            otp->type = g_strdup ("TOTP");
            otp->period = (guint32)json_integer_value (json_object_get (obj, "period"));
            if (otp->period == 0) {
                // andOTP exported backup for Steam might not contain the period field,
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

        const gchar *algo = json_string_value (json_object_get (obj, "algorithm"));
        if (g_ascii_strcasecmp (algo, "SHA1") == 0 ||
            g_ascii_strcasecmp (algo, "SHA256") == 0 ||
            g_ascii_strcasecmp (algo, "SHA512") == 0) {
                otp->algo = g_ascii_strup (algo, -1);
        } else {
            g_printerr ("algo not supported (must be either one of: sha1, sha256 or sha512\n");
            gcry_free (otp->secret);
            g_free (otp);
            json_decref (obj);
            return NULL;
        }

        otps = g_slist_append (otps, g_memdupX (otp, sizeof (otp_t)));
        g_free (otp);
    }

    json_decref (array);

    return otps;
}
