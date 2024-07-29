#include <glib.h>
#include "common.h"
#include "file-size.h"
#include "gquarks.h"

static void   parse_uri            (const gchar   *uri,
                                    GSList       **otps);

static void   parse_parameters     (const gchar   *modified_uri,
                                    otp_t         *otp);

static gchar *remove_null_encoding (const gchar   *uri);


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

    GString *uri = g_string_new (NULL);
    g_string_append (uri, "otpauth://");
    const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
    if (issuer != NULL && g_ascii_strcasecmp (issuer, "steam") == 0) {
        g_string_append (uri, "totp/");
        constructed_label = g_strconcat ("Steam:", json_string_value (json_object_get (obj, "label")), NULL);
    } else {
        g_string_append (uri, g_utf8_strdown (json_string_value (json_object_get (obj, "type")),  -1));
        g_string_append (uri, "/");
        if (issuer != NULL && g_utf8_strlen (issuer, -1) > 0) {
            constructed_label = g_strconcat (json_string_value (json_object_get (obj, "issuer")),
                                             ":",
                                             json_string_value (json_object_get (obj, "label")),
                                             NULL);
        } else {
            constructed_label = g_strdup (json_string_value (json_object_get (obj, "label")));
        }
    }

    gchar *escaped_label = g_uri_escape_string (constructed_label, NULL, FALSE);
    g_string_append (uri, escaped_label);
    g_string_append (uri, "?secret=");
    g_string_append (uri, json_string_value (json_object_get (obj, "secret")));
    if (issuer != NULL && g_ascii_strcasecmp (issuer, "steam") == 0) {
        g_string_append (uri, "&issuer=Steam");
    }

    gchar *escaped_issuer = NULL;
    if (issuer != NULL && g_utf8_strlen (issuer, -1) > 0) {
        g_string_append (uri, "&issuer=");
        escaped_issuer = g_uri_escape_string (json_string_value (json_object_get (obj, "issuer")), NULL, FALSE);
        g_string_append (uri,escaped_issuer);
    }

    gchar *str_to_append = NULL;
    g_string_append (uri, "&digits=");
    str_to_append = g_strdup_printf ("%lld", json_integer_value ( json_object_get (obj, "digits")));
    g_string_append (uri,str_to_append);
    g_free (str_to_append);
    g_string_append (uri, "&algorithm=");
    g_string_append (uri, json_string_value ( json_object_get (obj, "algo")));

    if (g_ascii_strcasecmp (json_string_value (json_object_get (obj, "type")), "TOTP") == 0) {
        g_string_append (uri, "&period=");
        str_to_append = g_strdup_printf ("%lld", json_integer_value ( json_object_get (obj, "period")));
        g_string_append (uri, str_to_append);
        g_free (str_to_append);
    } else {
        g_string_append (uri, "&counter=");
        str_to_append = g_strdup_printf ("%lld", json_integer_value ( json_object_get (obj, "counter")));
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

    gchar *sec_buf = gcry_calloc_secure (fs, 1);
    if (!g_file_get_contents (path, &sec_buf, NULL, err)) {
        g_set_error (err, generic_error_gquark(), GENERIC_ERRCODE, "Couldn't load the file content into memory.");
        gcry_free (sec_buf);
        return NULL;
    }

    set_otps_from_uris (sec_buf, &otps);

    gcry_free (sec_buf);

    return otps;
}


static void
parse_uri (const gchar   *uri,
           GSList       **otps)
{
    const gchar *uri_copy = uri;
    if (g_ascii_strncasecmp (uri_copy, "otpauth://", 10) != 0) {
        return;
    }
    uri_copy += 10;

    otp_t *otp = g_new0 (otp_t, 1);
    // set default digits value to 6. If something else is specified, it will be read later on
    otp->digits = 6;
    if (g_ascii_strncasecmp (uri_copy, "totp/", 5) == 0) {
        otp->type = g_strdup ("TOTP");
        otp->period = 30;
    } else if (g_ascii_strncasecmp (uri_copy, "hotp/", 5) == 0) {
        otp->type = g_strdup ("HOTP");
    } else {
        g_free (otp);
        return;
    }
    uri_copy += 5;

    if (g_strrstr (uri_copy, "algorithm") == NULL) {
        // if the uri doesn't contain the algo parameter, fallback to sha1
        otp->algo = g_strdup ("SHA1");
    }
    parse_parameters (uri_copy, otp);

    *otps = g_slist_append (*otps, g_memdup2 (otp, sizeof (otp_t)));
    g_free (otp);
}


static void
parse_parameters (const gchar   *modified_uri,
                  otp_t         *otp)
{
    // https://github.com/paolostivanin/OTPClient/issues/369#issuecomment-2238703716
    gchar *cleaned_uri = remove_null_encoding (modified_uri);
    gchar **tokens = g_strsplit (cleaned_uri, "?", -1);
    gchar *escaped_issuer_and_label = g_uri_unescape_string (tokens[0], NULL);
    gchar *mod_uri_copy_utf8 = g_utf8_offset_to_pointer (cleaned_uri, g_utf8_strlen (tokens[0], -1) + 1);
    g_strfreev (tokens);

    tokens = g_strsplit (escaped_issuer_and_label, ":", -1);
    if (tokens[0] && tokens[1]) {
        otp->issuer = g_strdup (g_strstrip (tokens[0]));
        otp->account_name = g_strdup (g_strstrip (tokens[1]));
    } else {
        otp->account_name = g_strdup (g_strstrip (tokens[0]));
    }
    g_free (escaped_issuer_and_label);
    g_strfreev (tokens);

    tokens = g_strsplit (mod_uri_copy_utf8, "&", -1);
    gint i = 0;
    while (tokens[i]) {
        if (g_ascii_strncasecmp (tokens[i], "secret=", 7) == 0) {
            tokens[i] += 7;
            otp->secret = secure_strdup (tokens[i]);
            tokens[i] -= 7;
        } else if (g_ascii_strncasecmp (tokens[i], "algorithm=", 10) == 0) {
            tokens[i] += 10;
            if (g_ascii_strcasecmp (tokens[i], "SHA1") == 0 ||
                g_ascii_strcasecmp (tokens[i], "SHA256") == 0 ||
                g_ascii_strcasecmp (tokens[i], "SHA512") == 0) {
                otp->algo = g_ascii_strup (tokens[i], -1);
            }
            tokens[i] -= 10;
        } else if (g_ascii_strncasecmp (tokens[i], "period=", 7) == 0) {
            tokens[i] += 7;
            otp->period = (guint8) g_ascii_strtoll (tokens[i], NULL, 10);
            tokens[i] -= 7;
        } else if (g_ascii_strncasecmp (tokens[i], "digits=", 7) == 0) {
            tokens[i] += 7;
            otp->digits = (guint8) g_ascii_strtoll (tokens[i], NULL, 10);
            tokens[i] -= 7;
        } else if (g_ascii_strncasecmp (tokens[i], "issuer=", 7) == 0) {
            tokens[i] += 7;
            if (!otp->issuer) {
                otp->issuer = g_strdup (g_strstrip (tokens[i]));
            }
            tokens[i] -= 7;
        } else if (g_ascii_strncasecmp (tokens[i], "counter=", 8) == 0) {
            tokens[i] += 8;
            otp->counter = (guint64)g_ascii_strtoll (tokens[i], NULL, 10);
            tokens[i] -= 8;
        }
        i++;
    }
    g_strfreev (tokens);
    g_free (cleaned_uri);
}


static gchar *
remove_null_encoding (const gchar *uri)
{
    GRegex *regex = g_regex_new ("%00", 0, 0, NULL);
    gchar *cleaned_uri = g_regex_replace_literal (regex, uri, -1, 0, "", 0, NULL);
    g_regex_unref (regex);

    return cleaned_uri;
}
