#define _DEFAULT_SOURCE
#include <string.h>
#include <glib.h>
#include "common.h"
#include "file-size.h"
#include "gquarks.h"
#include "otp-validation.h"

static void   parse_uri            (const gchar   *uri,
                                    GSList       **otps);

static void   free_parsed_otp      (otp_t         *otp);


void
set_otps_from_uris (const gchar   *otpauth_uris,
                    GSList       **otps)
{
    gchar **uris = g_strsplit (otpauth_uris, "\n", -1);
    guint i = 0, uris_len = g_strv_length (uris);
    gchar *haystack = NULL;
    if (uris_len > 0) {
        for (; i < uris_len; i++) {
            haystack = g_strrstr (uris[i], "otpauth");
            if (haystack != NULL) {
                parse_uri (haystack, otps);
            }
        }
    }
    g_strfreev (uris);
}


gchar *
get_otpauth_uri (json_t *obj)
{
    gchar *constructed_label = NULL;

    const gchar *type = json_string_value (json_object_get (obj, "type"));
    const gchar *label = json_string_value (json_object_get (obj, "label"));
    const gchar *secret = json_string_value (json_object_get (obj, "secret"));
    const gchar *algo = json_string_value (json_object_get (obj, "algo"));
    if (type == NULL || label == NULL || secret == NULL || algo == NULL) {
        return g_strdup ("");
    }

    GString *uri = g_string_new (NULL);
    g_string_append (uri, "otpauth://");
    const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
    if (issuer != NULL && g_ascii_strcasecmp (issuer, "steam") == 0) {
        g_string_append (uri, "totp/");
        constructed_label = g_strconcat ("Steam:", label, NULL);
    } else {
        gchar *type_lower = g_utf8_strdown (type, -1);
        g_string_append (uri, type_lower);
        g_free (type_lower);
        g_string_append (uri, "/");
        if (issuer != NULL && g_utf8_strlen (issuer, -1) > 0) {
            constructed_label = g_strconcat (issuer, ":", label, NULL);
        } else {
            constructed_label = g_strdup (label);
        }
    }

    gchar *escaped_label = g_uri_escape_string (constructed_label, NULL, FALSE);
    g_string_append (uri, escaped_label);
    g_string_append (uri, "?secret=");
    g_string_append (uri, secret);
    if (issuer != NULL && g_ascii_strcasecmp (issuer, "steam") == 0) {
        g_string_append (uri, "&issuer=Steam");
    }

    gchar *escaped_issuer = NULL;
    if (issuer != NULL && g_utf8_strlen (issuer, -1) > 0) {
        g_string_append (uri, "&issuer=");
        escaped_issuer = g_uri_escape_string (issuer, NULL, FALSE);
        g_string_append (uri, escaped_issuer);
    }

    gchar *str_to_append = NULL;
    g_string_append (uri, "&digits=");
    str_to_append = g_strdup_printf ("%lld", json_integer_value (json_object_get (obj, "digits")));
    g_string_append (uri, str_to_append);
    g_free (str_to_append);
    g_string_append (uri, "&algorithm=");
    g_string_append (uri, algo);

    if (g_ascii_strcasecmp (type, "TOTP") == 0) {
        g_string_append (uri, "&period=");
        str_to_append = g_strdup_printf ("%lld", json_integer_value (json_object_get (obj, "period")));
        g_string_append (uri, str_to_append);
        g_free (str_to_append);
    } else {
        g_string_append (uri, "&counter=");
        str_to_append = g_strdup_printf ("%lld", json_integer_value (json_object_get (obj, "counter")));
        g_string_append (uri, str_to_append);
        g_free (str_to_append);
    }

    g_string_append (uri, "\n");

    g_free (constructed_label);
    g_free (escaped_label);
    g_free (escaped_issuer);

    return g_string_free (uri, FALSE);
}


GSList *
get_otpauth_data (const gchar  *path,
                  gint32        max_file_size,
                  GError      **err)
{
    GSList *otps = NULL;
    goffset fs = get_file_size (path);
    if (fs < 10) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Couldn't get the file size (file doesn't exit or wrong file selected.");
        return NULL;
    }
    if (fs > max_file_size) {
        g_set_error (err, file_too_big_gquark (), FILE_TOO_BIG_ERRCODE, FILE_SIZE_SECMEM_MSG);
        return NULL;
    }

    gchar *file_buf = NULL;
    if (!g_file_get_contents (path, &file_buf, NULL, err)) {
        g_free (file_buf);
        return NULL;
    }

    gchar *sec_buf = gcry_calloc_secure ((gsize) fs + 1, 1);
    if (sec_buf == NULL) {
        g_set_error (err, secmem_alloc_error_gquark (), SECMEM_ALLOC_ERRCODE,
                     "Couldn't allocate secure memory for OTP URI import.");
        explicit_bzero (file_buf, fs);
        g_free (file_buf);
        return NULL;
    }
    memcpy (sec_buf, file_buf, fs);
    explicit_bzero (file_buf, fs);
    g_free (file_buf);

    set_otps_from_uris (sec_buf, &otps);

    gcry_free (sec_buf);

    return otps;
}


/* Real otpauth:// URIs are well under 1 KB. Cap at 4 KB so a malformed or
 * malicious backup file with a megabyte-long token can't drag the parser
 * through gigabytes of g_strsplit allocations. */
#define MAX_OTPAUTH_URI_LEN 4096

static void
parse_uri (const gchar   *uri,
           GSList       **otps)
{
    if (uri == NULL || g_ascii_strncasecmp (uri, "otpauth://", 10) != 0) {
        return;
    }
    if (strnlen (uri, MAX_OTPAUTH_URI_LEN + 1) > MAX_OTPAUTH_URI_LEN) {
        g_warning ("Skipping otpauth URI larger than %d bytes.", MAX_OTPAUTH_URI_LEN);
        return;
    }

    g_autoptr (GError) uri_err = NULL;
    GUri *parsed = g_uri_parse (uri, G_URI_FLAGS_NONE, &uri_err);
    if (parsed == NULL)
        return;

    const gchar *scheme = g_uri_get_scheme (parsed);
    const gchar *type_host = g_uri_get_host (parsed);
    const gchar *path = g_uri_get_path (parsed);
    const gchar *query = g_uri_get_query (parsed);
    if (g_ascii_strcasecmp (scheme, "otpauth") != 0 ||
        type_host == NULL || path == NULL || path[0] != '/' || path[1] == '\0' ||
        query == NULL || query[0] == '\0') {
        g_uri_unref (parsed);
        return;
    }

    g_autofree gchar *label_unescaped = g_uri_unescape_string (path + 1, NULL);
    if (label_unescaped == NULL) {
        g_uri_unref (parsed);
        return;
    }

    g_autoptr (GHashTable) params = g_uri_parse_params (query, -1, "&",
                                                        G_URI_PARAMS_NONE,
                                                        &uri_err);
    if (params == NULL) {
        g_uri_unref (parsed);
        return;
    }

    otp_t *otp = g_new0 (otp_t, 1);
    otp->digits = 6;
    otp->algo = g_strdup ("SHA1");
    if (g_ascii_strcasecmp (type_host, "totp") == 0) {
        otp->type = g_strdup ("TOTP");
        otp->period = 30;
    } else if (g_ascii_strcasecmp (type_host, "hotp") == 0) {
        otp->type = g_strdup ("HOTP");
    } else {
        free_parsed_otp (otp);
        g_uri_unref (parsed);
        return;
    }

    gchar **label_parts = g_strsplit (label_unescaped, ":", 2);
    if (label_parts[0] != NULL && label_parts[1] != NULL) {
        otp->issuer = g_strdup (g_strstrip (label_parts[0]));
        otp->account_name = g_strdup (g_strstrip (label_parts[1]));
    } else {
        otp->issuer = g_strdup ("");
        otp->account_name = g_strdup (g_strstrip (label_unescaped));
    }
    g_strfreev (label_parts);

    const gchar *issuer_param = g_hash_table_lookup (params, "issuer");
    if (issuer_param != NULL) {
        g_free (otp->issuer);
        otp->issuer = g_strdup (g_strstrip ((gchar *) issuer_param));
    }
    const gchar *secret = g_hash_table_lookup (params, "secret");
    otp->secret = secure_strdup (secret);

    const gchar *algo = g_hash_table_lookup (params, "algorithm");
    if (algo != NULL &&
        (g_ascii_strcasecmp (algo, "SHA1") == 0 ||
         g_ascii_strcasecmp (algo, "SHA256") == 0 ||
         g_ascii_strcasecmp (algo, "SHA512") == 0)) {
        g_free (otp->algo);
        otp->algo = g_ascii_strup (algo, -1);
    }

    const gchar *period = g_hash_table_lookup (params, "period");
    if (period != NULL) {
        gchar *endptr = NULL;
        gint64 v = g_ascii_strtoll (period, &endptr, 10);
        if (endptr != period && *endptr == '\0' && v >= OTP_PERIOD_MIN && v <= OTP_PERIOD_MAX)
            otp->period = (guint32) v;
    }
    const gchar *digits = g_hash_table_lookup (params, "digits");
    if (digits != NULL) {
        gchar *endptr = NULL;
        gint64 v = g_ascii_strtoll (digits, &endptr, 10);
        if (endptr != digits && *endptr == '\0' && v >= OTP_DIGITS_MIN && v <= OTP_DIGITS_MAX)
            otp->digits = (guint32) v;
    }
    const gchar *counter = g_hash_table_lookup (params, "counter");
    if (counter != NULL) {
        gchar *endptr = NULL;
        gint64 v = g_ascii_strtoll (counter, &endptr, 10);
        if (endptr != counter && *endptr == '\0' && v >= 0 &&
            (guint64) v < OTP_HOTP_COUNTER_MAX) {
            otp->counter = (guint64) v;
        }
    }

    GError *validation_err = NULL;
    /* Name an anonymous token rather than rejecting it (issue #462); the
     * commit path leaves it as-is once it carries a label. */
    otp_repair_anonymous_import_token (otp, g_slist_length (*otps));
    if (!otp_validate_import_token (otp, &validation_err)) {
        if (validation_err != NULL)
            g_clear_error (&validation_err);
        free_parsed_otp (otp);
    } else {
        *otps = g_slist_append (*otps, otp);
    }

    g_uri_unref (parsed);
}

static void
free_parsed_otp (otp_t *otp)
{
    if (otp == NULL)
        return;
    g_free (otp->type);
    g_free (otp->algo);
    g_free (otp->account_name);
    g_free (otp->issuer);
    gcry_free (otp->secret);
    g_free (otp->group);
    g_free (otp);
}
