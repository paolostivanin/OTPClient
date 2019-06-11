#include <glib.h>
#include "common.h"
#include "imports.h"


static void parse_uri           (const gchar   *uri,
                                 GSList       **otps);

static void parse_parameters    (const gchar   *modified_uri,
                                 otp_t         *otp);


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
        return;
    }
    uri_copy += 5;

    if (g_strrstr (uri_copy, "algorithm") == NULL) {
        // if the uri doesn't contain the algo parameter, fallback to sha1
        otp->algo = g_strdup ("SHA1");
    }
    parse_parameters (uri_copy, otp);

    *otps = g_slist_append (*otps, g_memdup (otp, sizeof (otp_t)));
    g_free (otp);
}


static void
parse_parameters (const gchar   *modified_uri,
                  otp_t         *otp)
{
    const gchar *mod_uri_copy = modified_uri;
    gchar **tokens = g_strsplit (mod_uri_copy, "?", -1);
    gchar *escaped_issuer_and_label = g_uri_unescape_string (tokens[0], NULL);
    mod_uri_copy += g_utf8_strlen (tokens[0], -1) + 1; // "issuer:label?"
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

    tokens = g_strsplit (mod_uri_copy, "&", -1);
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
}
