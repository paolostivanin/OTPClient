#include <glib.h>
#include <errno.h>
#include <zip.h>
#include "imports.h"

#define BUF_SIZE 2097152  // 2 MiB

static void parse_content (const gchar *buf, GSList **otps);

static void parse_uri (const gchar *uri, GSList **otps);

static void parse_parameters (const gchar *modified_uri, otp_t *otp);


GSList *
get_encrypted_zip_content (const gchar *zip_path, const gchar *password)
{
    zip_t *zip_file;
    struct zip_file *zf;
    struct zip_stat sb;

    gchar *buf = g_malloc0 (BUF_SIZE);
    if (!buf) {
        // TODO error
        return NULL;
    }
    int zip_err;
    if ((zip_file = zip_open (zip_path, ZIP_RDONLY, &zip_err)) == NULL) {
        zip_error_to_str (buf, sizeof (buf), zip_err, errno);
        g_printerr ("couldn't open zip file '%s': %s\n", zip_path, buf);
        return NULL;
    }

    zip_set_default_password (zip_file, password);

    if (zip_stat (zip_file, "Accounts.txt", 0, &sb) < 0) {
        g_printerr ("zip_stat failed\n");
        zip_discard (zip_file);
        return NULL;
    }

    zf = zip_fopen (zip_file, "Accounts.txt", 0);
    if (zf == NULL) {
        g_printerr ("zip_fopen failed\n");
        zip_discard (zip_file);
        return NULL;
    }

    zip_int64_t len = zip_fread (zf, buf, BUF_SIZE);
    if (len < 0) {
        g_printerr ("zip_fread failed\n");
        zip_fclose (zf);
        zip_discard (zip_file);
        return NULL;
    }

    GSList *otps = NULL;
    parse_content (buf, &otps);

    zip_fclose (zf);

    zip_discard (zip_file);

    // TODO before calling slist_free_full, secret, label and issuer must be freed
    return otps;
}


static void
parse_content (const gchar *buf, GSList **otps)
{
    gchar **uris = g_strsplit (buf, "\n", -1);
    gint i = 0;
    while (uris[i]) {
        parse_uri (uris[i], &(*otps));
        i++;
    }
    g_strfreev (uris);
}


static void
parse_uri (const gchar *uri, GSList **otps)
{
    const gchar *uri_copy = uri;
    if (g_ascii_strncasecmp (uri_copy, "otpauth://", 10) != 0) {
        return;
    }
    uri_copy += 10;

    otp_t *otp = g_new0 (otp_t, 1);
    if (g_ascii_strncasecmp (uri_copy, "totp/", 5) == 0) {
        otp->type = TYPE_TOTP;
        otp->period = 30;
    } else if (g_ascii_strncasecmp (uri_copy, "htop/", 5) == 0) {
        otp->type = TYPE_HOTP;
        otp->counter = 0;
    } else {
        return;
    }
    uri_copy += 5;

    parse_parameters (uri_copy, otp);

    *otps = g_slist_append (*otps, g_memdup (otp, sizeof (otp_t)));
    g_free (otp);
}


static void
parse_parameters (const gchar *modified_uri, otp_t *otp)
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
            otp->secret = g_strdup (tokens[i]);
            tokens[i] -= 7;
        } else if (g_ascii_strncasecmp (tokens[i], "algorithm=", 10) == 0) {
            tokens[i] += 10;
            if (g_ascii_strcasecmp (tokens[i], "SHA256") == 0) {
                otp->algo = ALGO_SHA256;
            } else if (g_ascii_strcasecmp (tokens[i], "SHA512") == 0) {
                otp->algo = ALGO_SHA512;
            } else {
                otp->algo = ALGO_SHA1;
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
            otp->counter = (guint8) g_ascii_strtoll (tokens[i], NULL, 10);
            tokens[i] -= 8;
        }
        i++;
    }
    g_strfreev (tokens);
}