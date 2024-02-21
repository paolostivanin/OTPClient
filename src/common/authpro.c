#include <glib.h>
#include <gio/gio.h>
#include <gcrypt.h>
#include "common.h"
#include "../gquarks.h"
#include "../imports.h"

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
            otps = g_slist_append (otps, g_memdup2 (otp, sizeof (otp_t)));
        }

        gcry_free (otp->secret);
        g_free (otp->issuer);
        g_free (otp->account_name);
        g_free (otp->algo);
        g_free (otp->type);
        g_free (otp);
    }

    json_decref (root);

    return otps;
}
