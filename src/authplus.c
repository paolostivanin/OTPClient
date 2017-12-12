#include <glib.h>
#include <errno.h>
#include <zip.h>
#include <gcrypt.h>
#include "imports.h"
#include "common.h"
#include "gquarks.h"
#include "otpclient.h"

static void parse_content (const gchar *buf, GSList **otps);

static void parse_uri (const gchar *uri, GSList **otps);

static void parse_parameters (const gchar *modified_uri, otp_t *otp);


GSList *
get_authplus_data (const gchar   *zip_path,
                   const gchar   *password,
                   GError       **err)
{
    zip_t *zip_file;
    struct zip_file *zf;
    struct zip_stat sb;

    gchar buf[128];
    int zip_err;
    if ((zip_file = zip_open (zip_path, ZIP_RDONLY, &zip_err)) == NULL) {
        zip_error_to_str (buf, MAX_FILE_SIZE, zip_err, errno);
        gchar *msg = g_strdup_printf ("Couldn't open zip file '%s': %s", zip_path, buf);
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "%s", msg);
        g_free (msg);
        return NULL;
    }

    zip_set_default_password (zip_file, password);

    if (zip_stat (zip_file, "Accounts.txt", 0, &sb) < 0) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "%s", "zip_stat failed");
        zip_discard (zip_file);
        return NULL;
    }

    gchar *sec_buf = gcry_malloc_secure (sb.size);
    if (sec_buf == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Couldn't allocate secure memory");
        return NULL;
    }
    zf = zip_fopen (zip_file, "Accounts.txt", 0);
    if (zf == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "%s", "zip_fopen failed");
        zip_discard (zip_file);
        return NULL;
    }

    zip_int64_t len = zip_fread (zf, sec_buf, sb.size);
    if (len < 0) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "%s", "zip_fread failed");
        zip_fclose (zf);
        zip_discard (zip_file);
        return NULL;
    }

    GSList *otps = NULL;
    parse_content (sec_buf, &otps);

    zip_fclose (zf);

    zip_discard (zip_file);

    gcry_free (sec_buf);

    return otps;
}


static void
parse_content (const gchar   *buf,
               GSList       **otps)
{
    gchar **uris = g_strsplit (buf, "\n", -1);
    gint i = 0;
    while (g_strrstr (uris[i], "otpauth") != NULL) {
        parse_uri (uris[i], &(*otps));
        i++;
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
        otp->issuer = g_strdup (tokens[0]);
        otp->label = g_strdup (tokens[1]);
    } else {
        otp->label = g_strdup (tokens[0]);
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
                otp->issuer = g_strdup (tokens[i]);
            }
            tokens[i] -= 7;
        } else if (g_ascii_strncasecmp (tokens[i], "counter=", 8) == 0) {
            tokens[i] += 8;
            otp->counter = g_ascii_strtoll (tokens[i], NULL, 10);
            tokens[i] -= 8;
        }
        i++;
    }
    g_strfreev (tokens);
}
