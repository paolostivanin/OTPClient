#define _DEFAULT_SOURCE
#include <string.h>
#include <glib.h>
#include <glib/gi18n.h>
#include "common.h"
#include "file-size.h"
#include "gquarks.h"
#include "otp-validation.h"
#include "parse-uri.h"

static void   parse_uri            (const gchar   *uri,
                                    GSList       **otps, OtpImportDiagnostics *diagnostics, guint source_index);

static void   free_parsed_otp      (otp_t         *otp);


void
set_otps_from_uris_full (const gchar *otpauth_uris, GSList **otps, OtpImportDiagnostics *diagnostics)
{
    gchar **uris = g_strsplit (otpauth_uris, "\n", -1);
    guint i = 0, uris_len = g_strv_length (uris);
    gchar *haystack = NULL;
    if (uris_len > 0) {
        for (; i < uris_len; i++) {
            haystack = g_strrstr (uris[i], "otpauth");
            if (haystack != NULL) {
                parse_uri (haystack, otps, diagnostics, i);
            } else if (g_strstrip (uris[i])[0] != '\0' && uris[i][0] != '#') {
                otp_import_diagnostics_add (diagnostics, i, _("Not an OTP URI."));
            }
        }
    }
    /* g_strsplit copied every line (secrets included) into ordinary heap.
     * Wipe those copies before releasing them, matching the project's wipe
     * discipline for secret material. */
    for (guint k = 0; k < uris_len; k++)
        explicit_bzero (uris[k], strlen (uris[k]));
    g_strfreev (uris);
}


gchar *
get_otpauth_uri (json_t *obj)
{
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
    // Steam tokens are TOTP with a Steam-specific alphabet; every reader keys
    // off the issuer, so the type in the URI is plain totp.
    gboolean is_steam = (issuer != NULL && g_ascii_strcasecmp (issuer, "steam") == 0);
    if (is_steam) {
        g_string_append (uri, "totp/");
    } else {
        gchar *type_lower = g_utf8_strdown (type, -1);
        g_string_append (uri, type_lower);
        g_free (type_lower);
        g_string_append (uri, "/");
    }

    // Escape the two halves of the label separately and join them with a
    // literal colon. Escaping the joined string instead turns the separator
    // into %3A, which a reader cannot tell apart from a colon that belongs to
    // the issuer or to the account name, so "Acme:Corp:alice" comes back split
    // in the wrong place.
    const gchar *uri_issuer = is_steam ? "Steam" : issuer;
    gchar *escaped_issuer = NULL;
    if (uri_issuer != NULL && uri_issuer[0] != '\0') {
        escaped_issuer = g_uri_escape_string (uri_issuer, NULL, FALSE);
        g_string_append (uri, escaped_issuer);
        g_string_append_c (uri, ':');
    }
    gchar *escaped_label = g_uri_escape_string (label, NULL, FALSE);
    g_string_append (uri, escaped_label);
    g_string_append (uri, "?secret=");
    g_string_append (uri, secret);

    if (escaped_issuer != NULL) {
        g_string_append (uri, "&issuer=");
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

    g_free (escaped_label);
    g_free (escaped_issuer);

    return g_string_free (uri, FALSE);
}


GSList *
get_otpauth_data_full (const gchar  *path,
                  gint32        max_file_size,
                  OtpImportDiagnostics *diagnostics, GError      **err)
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

    /* Everything past here sizes itself on what was actually read, never on the
     * stat above: the file can be rewritten shorter between the two, and
     * trusting the stale size is a heap over-read followed by an out-of-bounds
     * wipe. The stat is only a cheap way to refuse an oversized file early. */
    gchar *file_buf = NULL;
    gsize file_len = 0;
    if (!g_file_get_contents (path, &file_buf, &file_len, err)) {
        g_free (file_buf);
        return NULL;
    }
    if (file_len > (gsize) max_file_size) {
        g_set_error (err, file_too_big_gquark (), FILE_TOO_BIG_ERRCODE, FILE_SIZE_SECMEM_MSG);
        explicit_bzero (file_buf, file_len);
        g_free (file_buf);
        return NULL;
    }

    gchar *sec_buf = gcry_calloc_secure (file_len + 1, 1);
    if (sec_buf == NULL) {
        g_set_error (err, secmem_alloc_error_gquark (), SECMEM_ALLOC_ERRCODE,
                     "Couldn't allocate secure memory for OTP URI import.");
        explicit_bzero (file_buf, file_len);
        g_free (file_buf);
        return NULL;
    }
    memcpy (sec_buf, file_buf, file_len);
    explicit_bzero (file_buf, file_len);
    g_free (file_buf);

    set_otps_from_uris_full (sec_buf, &otps, diagnostics);

    gcry_free (sec_buf);

    return otps;
}


/* Real otpauth:// URIs are well under 1 KB. Cap at 4 KB so a malformed or
 * malicious backup file with a megabyte-long token can't drag the parser
 * through gigabytes of g_strsplit allocations. */
#define MAX_OTPAUTH_URI_LEN 4096


/* Split a still-encoded label into its issuer and account halves and decode
 * each of them. The separator is a literal colon, so a colon inside either half
 * arrives as %3A and stays where it belongs.
 *
 * Exporters that encode the separator too, ours included before 5.2.0, are
 * still read: with no literal colon to go by, the first %3A is taken as the
 * separator. That is a guess, and for a label whose issuer itself contains a
 * colon it is the wrong one, but it is the same guess those files were written
 * with and the issuer parameter usually corrects the issuer half anyway.
 *
 * Returns FALSE, leaving both out-parameters NULL, when a half is not valid
 * percent-encoding. */
static gboolean
split_escaped_label (const gchar  *escaped_label,
                     gchar       **issuer,
                     gchar       **account_name)
{
    const gchar *sep = strchr (escaped_label, ':');
    gsize sep_len = 1;
    if (sep == NULL) {
        for (const gchar *p = escaped_label; (p = strchr (p, '%')) != NULL; p++) {
            if (g_ascii_strncasecmp (p, "%3a", 3) == 0) {
                sep = p;
                sep_len = 3;
                break;
            }
        }
    }

    if (sep == NULL) {
        *issuer = g_strdup ("");
        *account_name = g_uri_unescape_string (escaped_label, NULL);
    } else {
        g_autofree gchar *raw_issuer = g_strndup (escaped_label, (gsize) (sep - escaped_label));
        *issuer = g_uri_unescape_string (raw_issuer, NULL);
        *account_name = g_uri_unescape_string (sep + sep_len, NULL);
    }

    if (*issuer == NULL || *account_name == NULL) {
        g_clear_pointer (issuer, g_free);
        g_clear_pointer (account_name, g_free);
        return FALSE;
    }
    g_strstrip (*issuer);
    g_strstrip (*account_name);
    return TRUE;
}

static void
parse_uri (const gchar   *uri,
           GSList       **otps, OtpImportDiagnostics *diagnostics, guint source_index)
{
    if (uri == NULL || g_ascii_strncasecmp (uri, "otpauth://", 10) != 0) {
        otp_import_diagnostics_add (diagnostics, source_index, _("Malformed or unsupported OTP URI."));
        return;
    }
    if (strnlen (uri, MAX_OTPAUTH_URI_LEN + 1) > MAX_OTPAUTH_URI_LEN) {
        g_warning ("Skipping otpauth URI larger than %d bytes.", MAX_OTPAUTH_URI_LEN);
        otp_import_diagnostics_add (diagnostics, source_index, _("Malformed or unsupported OTP URI."));
        return;
    }

    /* G_URI_FLAGS_ENCODED keeps every component percent-encoded, which is what
     * lets the label be split before it is decoded and the query parameters be
     * decoded exactly once, by g_uri_parse_params below. Decoding here instead
     * would decode both of them twice: an account name holding a literal '%'
     * becomes a broken escape and the whole token is refused, and an encoded
     * separator turns into a real one and lands in the wrong half. */
    g_autoptr (GError) uri_err = NULL;
    GUri *parsed = g_uri_parse (uri, G_URI_FLAGS_ENCODED, &uri_err);
    if (parsed == NULL) {
        otp_import_diagnostics_add (diagnostics, source_index, _("Malformed or unsupported OTP URI."));
        return;
    }

    const gchar *scheme = g_uri_get_scheme (parsed);
    const gchar *type_host = g_uri_get_host (parsed);
    const gchar *path = g_uri_get_path (parsed);
    const gchar *query = g_uri_get_query (parsed);
    if (g_ascii_strcasecmp (scheme, "otpauth") != 0 ||
        type_host == NULL || path == NULL || path[0] != '/' || path[1] == '\0' ||
        query == NULL || query[0] == '\0') {
        g_uri_unref (parsed);
        otp_import_diagnostics_add (diagnostics, source_index, _("Malformed or unsupported OTP URI."));
        return;
    }

    g_autoptr (GHashTable) params = g_uri_parse_params (query, -1, "&",
                                                        G_URI_PARAMS_NONE,
                                                        &uri_err);
    if (params == NULL) {
        g_uri_unref (parsed);
        otp_import_diagnostics_add (diagnostics, source_index, _("Malformed or unsupported OTP URI."));
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
        otp_import_diagnostics_add (diagnostics, source_index, _("Malformed or unsupported OTP URI."));
        return;
    }

    if (!split_escaped_label (path + 1, &otp->issuer, &otp->account_name)) {
        free_parsed_otp (otp);
        g_uri_unref (parsed);
        otp_import_diagnostics_add (diagnostics, source_index, _("Malformed or unsupported OTP URI."));
        return;
    }

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
        otp_import_diagnostics_add (diagnostics, source_index, validation_err != NULL ? validation_err->message : _("Invalid token."));
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

void
set_otps_from_uris (const gchar *uris, GSList **otps)
{
    set_otps_from_uris_full (uris, otps, NULL);
}

GSList *
get_otpauth_data (const gchar *path, gint32 max_file_size, GError **err)
{
    return get_otpauth_data_full (path, max_file_size, NULL, err);
}
