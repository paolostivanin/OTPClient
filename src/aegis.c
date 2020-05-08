#include <glib.h>
#include <gcrypt.h>
#include <jansson.h>
#include <time.h>
#include "imports.h"
#include "gui-common.h"
#include "gquarks.h"


static GSList *parse_json_data                (const gchar          *data,
                                               GError              **err);


GSList *
get_aegis_data (const gchar     *path,
                GError         **err)
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
export_aegis (const gchar *export_path,
              json_t *json_db_data)
{
    GError *err = NULL;
    json_t *root = json_object ();
    json_object_set (root, "version", json_integer(1));

    json_t *aegis_header_obj = json_object ();
    json_object_set (aegis_header_obj, "slots", json_null ());
    json_object_set (aegis_header_obj, "params", json_null ());
    json_object_set (root, "header", aegis_header_obj);

    json_t *aegis_db_obj = json_object ();
    json_t *array = json_array ();
    json_object_set (aegis_db_obj, "version", json_integer(1));
    json_object_set (aegis_db_obj, "entries", array);
    json_object_set (root, "db", aegis_db_obj);

    json_t *db_obj, *export_obj, *info_obj;
    gsize index;
    json_array_foreach (json_db_data, index, db_obj) {
        export_obj = json_object ();
        info_obj = json_object ();
        json_t *otp_type = json_object_get (db_obj, "type");

        const gchar *issuer = json_string_value (json_object_get (db_obj, "issuer"));
        // TODO: must verify this
        if (issuer != NULL && g_ascii_strcasecmp (issuer, "steam") == 0) {
            json_object_set (export_obj, "type", json_string ("STEAM"));
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

    FILE *fp = fopen (export_path, "w");
    if (fp == NULL) {
        g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "couldn't create the file object");
    } else {
        if (json_dumpf (root, fp, JSON_COMPACT) == -1) {
            g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "couldn't dump json data to file");
        }
        fclose (fp);
    }

    json_array_clear (array);
    json_decref (aegis_db_obj);
    json_decref (aegis_header_obj);
    json_decref (root);

    return (err != NULL ? g_strdup (err->message) : NULL);
}


static GSList *
parse_json_data (const gchar *data,
                 GError     **err)
{
    json_error_t jerr;
    json_t *root = json_loads (data, 0, &jerr);
    if (root == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "%s", jerr.text);
        return NULL;
    }
    json_t *array = json_object_get(json_object_get(root, "db"), "entries");
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
            // TODO: must verify this
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

        otps = g_slist_append (otps, g_memdup (otp, sizeof (otp_t)));
        g_free (otp);
    }

    json_decref (root);

    return otps;
}
