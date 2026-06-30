#define _GNU_SOURCE
#include <glib.h>
#include <glib/gi18n.h>
#include <gio/gio.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
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
    // Prevent secrets from leaking into core dumps if the process crashes.
    // Best-effort: a failure here is non-fatal but worth a warning.
    if (prctl (PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
        g_warning ("Failed to disable core dumps via PR_SET_DUMPABLE; secrets may leak on crash.");
    }
    struct rlimit core_limit = { 0, 0 };
    if (setrlimit (RLIMIT_CORE, &core_limit) != 0) {
        g_warning ("Failed to set RLIMIT_CORE to 0; core dumps may still be produced on crash.");
    }

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
    if (g_strcmp0 (algo, "SHA1") == 0)
        return COTP_SHA1;
    if (g_strcmp0 (algo, "SHA256") == 0)
        return COTP_SHA256;
    if (g_strcmp0 (algo, "SHA512") == 0)
        return COTP_SHA512;
    g_warning ("Unknown OTP algorithm '%s'; defaulting to SHA1.",
               algo != NULL ? algo : "(null)");
    return COTP_SHA1;
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

void
sensitive_free (gchar *value)
{
    if (value == NULL)
        return;
    explicit_bzero (value, strlen (value));
    free (value);
}

void
sensitive_g_free (gchar *value)
{
    if (value == NULL)
        return;
    explicit_bzero (value, strlen (value));
    g_free (value);
}

void
sensitive_secure_free (gchar *value)
{
    if (value == NULL)
        return;
    explicit_bzero (value, strlen (value));
    gcry_free (value);
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
        return NULL;
    }

    size_t out_len = len / 2;
    guchar *bytes = (guchar *) g_malloc (out_len + 1);
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


guchar *
hexstr_to_bytes_exact (const gchar *hexstr,
                       gsize        expected_len)
{
    if (hexstr == NULL)
        return NULL;

    size_t hex_len = strlen (hexstr);
    if (expected_len > (G_MAXSIZE / 2) || hex_len != expected_len * 2)
        return NULL;

    return hexstr_to_bytes (hexstr);
}


gchar *
bytes_to_hexstr (const guchar *data, size_t datalen)
{
    gchar hex_str[]= "0123456789abcdef";

    if (data == NULL && datalen > 0) {
        return NULL;
    }
    if (datalen > (G_MAXSIZE - 1) / 2) {
        return NULL;
    }

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
    if (derived_key == NULL) {
        return NULL;
    }
    // taglen, iterations, memory_cost (65536=64MiB), parallelism
    const unsigned long params[4] = {32, 3, 65536, 4};
    gcry_kdf_hd_t hd;
    // gcry_kdf_open expects the password length in BYTES, not Unicode characters.
    if (gcry_kdf_open (&hd, GCRY_KDF_ARGON2, GCRY_KDF_ARGON2ID,
                       params, 4,
                       password, strlen (password),
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
    gsize bytes_done = 0;
    if (!g_input_stream_read_all (G_INPUT_STREAM (in_stream), salt, salt_size,
                                  &bytes_done, NULL, err) ||
        bytes_done != (gsize) salt_size) {
        if (err != NULL && *err == NULL)
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Short read while reading encrypted backup salt.");
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    }

    g_autofree guchar *iv = g_malloc0 (iv_size);
    if (!g_input_stream_read_all (G_INPUT_STREAM (in_stream), iv, iv_size,
                                  &bytes_done, NULL, err) ||
        bytes_done != (gsize) iv_size) {
        if (err != NULL && *err == NULL)
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Short read while reading encrypted backup IV.");
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    }

    goffset input_file_size = get_file_size (path);
    gint32 offset = 0;
    switch (provider) {
        case AUTHPRO:
            // 16 is the size of the header
            offset = 16;
            break;
    }
    // Validate size in signed math BEFORE casting to gsize. A truncated or
    // tampered backup with input_file_size < (offset+salt+iv+tag+1) would
    // otherwise produce a huge unsigned enc_buf_size that bypasses the
    // "non-encrypted" check and either OOM-aborts g_malloc0 or produces a
    // misleading "file too big" error.
    goffset min_size = (goffset) offset + salt_size + iv_size + tag_size + 1;
    if (input_file_size < min_size) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Selected file is too small to be a valid encrypted backup (got %" G_GOFFSET_FORMAT " bytes, need at least %" G_GOFFSET_FORMAT ").",
                     input_file_size, min_size);
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    }
    goffset payload_size = input_file_size - offset - salt_size - iv_size - tag_size;
    if (payload_size > max_file_size) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        g_set_error (err, file_too_big_gquark (), FILE_TOO_BIG_ERRCODE, FILE_SIZE_SECMEM_MSG);
        return NULL;
    }
    gsize enc_buf_size = (gsize) payload_size;

    if (!g_seekable_seek (G_SEEKABLE (in_stream), input_file_size - tag_size, G_SEEK_SET, NULL, err)) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    }
    g_autofree guchar *tag = g_malloc0 (tag_size);
    if (!g_input_stream_read_all (G_INPUT_STREAM (in_stream), tag, tag_size,
                                  &bytes_done, NULL, err) ||
        bytes_done != (gsize) tag_size) {
        if (err != NULL && *err == NULL)
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Short read while reading encrypted backup tag.");
        g_object_unref (in_stream);
        g_object_unref (in_file);
        return NULL;
    }

    guchar *enc_buf = g_malloc0 (enc_buf_size);
    if (!g_seekable_seek (G_SEEKABLE(in_stream), offset + salt_size + iv_size, G_SEEK_SET, NULL, err)) {
        g_object_unref (in_stream);
        g_object_unref (in_file);
        g_free (enc_buf);
        return NULL;
    }
    if (!g_input_stream_read_all (G_INPUT_STREAM (in_stream), enc_buf, enc_buf_size,
                                  &bytes_done, NULL, err) ||
        bytes_done != enc_buf_size) {
        if (err != NULL && *err == NULL)
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Short read while reading encrypted backup payload.");
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

    gchar *decrypted_data = gcry_calloc_secure (enc_buf_size + 1, 1);
    if (decrypted_data == NULL) {
        g_set_error (err, secmem_alloc_error_gquark (), SECMEM_ALLOC_ERRCODE,
                     "Couldn't allocate %" G_GSIZE_FORMAT " bytes of secure memory for decryption.",
                     enc_buf_size);
        gcry_cipher_close (hd);
        g_free (enc_buf);
        gcry_free (derived_key);
        return NULL;
    }
    gpg_error_t gpg_err = gcry_cipher_decrypt (hd, decrypted_data, enc_buf_size, enc_buf, enc_buf_size);
    if (gpg_err) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Decryption failed: %s/%s", gcry_strsource (gpg_err), gcry_strerror (gpg_err));
        g_free (enc_buf);
        gcry_free (derived_key);
        gcry_free (decrypted_data);
        gcry_cipher_close (hd);
        return NULL;
    }
    // Check for any tag-verification error, not just CHECKSUM. A non-CHECKSUM
    // failure (INV_LENGTH, NO_KEY, ...) would otherwise fall through and we'd
    // return unverified plaintext as if it were authentic.
    gpg_error_t tag_err = gcry_cipher_checktag (hd, tag, tag_size);
    if (tag_err != GPG_ERR_NO_ERROR) {
        if (gcry_err_code (tag_err) == GPG_ERR_CHECKSUM) {
            g_set_error (err, bad_tag_gquark (), BAD_TAG_ERRCODE, "Either the file is corrupted or the password is wrong");
        } else {
            g_set_error (err, bad_tag_gquark (), BAD_TAG_ERRCODE,
                         "Tag verification failed: %s/%s", gcry_strsource (tag_err), gcry_strerror (tag_err));
        }
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
    // GString grows as needed, so a long label/issuer no longer gets truncated
    // and inflates the collision rate of the 32-bit jenkins hash. The hash is
    // still only used as a fast first-pass filter - add_otps_to_db_ex breaks
    // ties with json_equal so a colliding-but-distinct entry isn't silently
    // dropped as a "duplicate".
    GString *tmp_string = g_string_new (NULL);
    const gchar *key;
    json_t *value;
    json_object_foreach (obj, key, value) {
        if (g_strcmp0 (key, "group") == 0)
            continue;
        if (g_strcmp0 (key, "period") == 0 || g_strcmp0 (key, "counter") == 0 || g_strcmp0 (key, "digits") == 0) {
            json_int_t v = json_integer_value (value);
            g_string_append_printf (tmp_string, "%" G_GINT64_FORMAT, (gint64) v);
        } else {
            const gchar *str_val = json_string_value (value);
            if (str_val != NULL)
                g_string_append (tmp_string, str_val);
        }
    }

    guint32 hash = jenkins_one_at_a_time_hash (tmp_string->str, tmp_string->len + 1);

    // Wipe before free: tmp_string holds the OTP secret as part of the hashed
    // material. GString uses regular g_malloc, so explicit_bzero is needed to
    // avoid leaving secret bytes on the freed heap page.
    if (tmp_string->str != NULL && tmp_string->allocated_len > 0)
        explicit_bzero (tmp_string->str, tmp_string->allocated_len);
    g_string_free (tmp_string, TRUE);

    return hash;
}


void
free_otps_gslist (GSList *otps,
                  guint   list_len G_GNUC_UNUSED)
{
    for (GSList *l = otps; l != NULL; l = l->next) {
        otp_t *otp_data = l->data;
        if (otp_data == NULL)
            continue;
        g_free (otp_data->type);
        g_free (otp_data->algo);
        g_free (otp_data->account_name);
        g_free (otp_data->issuer);
        gcry_free (otp_data->secret);
        g_free (otp_data->group);
        // The struct itself is heap-allocated by every producer (parse-uri
        // via g_memdup2, importers via g_new0). Freeing only the inner fields
        // leaked sizeof(otp_t) per parsed token on every URI / import.
        g_free (otp_data);
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
                guint64      ctr,
                const gchar *group)
{
    // set_new (not set) for fresh literals: set increments the refcount, but
    // these literals have no local handle to decref afterwards, so the extra
    // reference would leak when the parent object is eventually freed.
    json_t *obj = json_object ();
    json_object_set_new (obj, "type", json_string (type != NULL ? type : ""));
    json_object_set_new (obj, "label", json_string (acc_label != NULL ? acc_label : ""));
    json_object_set_new (obj, "issuer", json_string (acc_iss != NULL ? acc_iss : ""));
    json_object_set_new (obj, "digits", json_integer (digits));
    json_object_set_new (obj, "algo", json_string (algo));

    json_object_set_new (obj, "secret", json_string (acc_key != NULL ? acc_key : ""));

    if (g_ascii_strcasecmp (type, "TOTP") == 0) {
        json_object_set_new (obj, "period", json_integer (period));
    } else {
        json_object_set_new (obj, "counter", json_integer ((json_int_t)ctr));
    }

    if (group != NULL && group[0] != '\0')
        json_object_set_new (obj, "group", json_string (group));

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


gboolean
output_stream_write_all_exact (GOutputStream  *stream,
                               const void     *buffer,
                               gsize           count,
                               GError        **err)
{
    gsize bytes_written = 0;
    if (!g_output_stream_write_all (stream, buffer, count, &bytes_written, NULL, err))
        return FALSE;
    if (bytes_written != count) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Short write: wrote %" G_GSIZE_FORMAT " of %" G_GSIZE_FORMAT " bytes.",
                     bytes_written, count);
        return FALSE;
    }
    return TRUE;
}


int
path_open_safe_regular_file (const gchar  *path,
                             GError      **err)
{
    if (path == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "No path supplied.");
        return -1;
    }
    int fd = open (path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Refusing to open '%s': %s", path,
                     errno == ELOOP ? "is a symlink" : g_strerror (errno));
        return -1;
    }
    struct stat st;
    if (fstat (fd, &st) < 0) {
        int saved_errno = errno;
        close (fd);
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Cannot stat '%s': %s", path, g_strerror (saved_errno));
        return -1;
    }
    if (!S_ISREG (st.st_mode)) {
        close (fd);
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "'%s' is not a regular file.", path);
        return -1;
    }
    return fd;
}
