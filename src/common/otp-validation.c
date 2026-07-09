#include <glib.h>
#include <jansson.h>
#include <ctype.h>
#include "gquarks.h"
#include "otp-validation.h"

static gboolean
is_supported_algo (const gchar *algo)
{
    return algo != NULL &&
        (g_ascii_strcasecmp (algo, "SHA1") == 0 ||
         g_ascii_strcasecmp (algo, "SHA256") == 0 ||
         g_ascii_strcasecmp (algo, "SHA512") == 0);
}

gboolean
otp_secret_is_valid_base32 (const gchar *secret)
{
    if (secret == NULL || secret[0] == '\0')
        return FALSE;

    gboolean seen_padding = FALSE;
    for (const gchar *p = secret; *p != '\0'; p++) {
        if (g_ascii_isspace (*p))
            continue;
        if (*p == '=') {
            seen_padding = TRUE;
            continue;
        }
        if (seen_padding)
            return FALSE;
        gchar c = g_ascii_toupper (*p);
        if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7')))
            return FALSE;
    }
    return TRUE;
}

/* Placeholder name for a token that arrives with neither a label nor an issuer.
 * Not translated: it is written into stored token data and must stay stable
 * regardless of the locale the database is later opened under. The index keeps
 * multiple placeholders distinct and mirrors the "Token N" position that
 * validation reports, so the label matches the diagnostic users used to see. */
static gchar *
otp_anonymous_placeholder (gsize index)
{
    return g_strdup_printf ("Unknown %" G_GSIZE_FORMAT, index);
}

gboolean
otp_validate_token_object (json_t  *obj,
                           gsize    index,
                           GError **err)
{
    if (!json_is_object (obj)) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Token %" G_GSIZE_FORMAT " is not an object.", index);
        return FALSE;
    }

    const gchar *type = json_string_value (json_object_get (obj, "type"));
    const gchar *label = json_string_value (json_object_get (obj, "label"));
    const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
    const gchar *secret = json_string_value (json_object_get (obj, "secret"));
    const gchar *algo = json_string_value (json_object_get (obj, "algo"));
    json_t *digits_obj = json_object_get (obj, "digits");

    if (type == NULL || (g_ascii_strcasecmp (type, "TOTP") != 0 &&
                         g_ascii_strcasecmp (type, "HOTP") != 0)) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Token %" G_GSIZE_FORMAT " has an unsupported or missing type.", index);
        return FALSE;
    }
    /* A token only needs one human-readable name. Issuer-only tokens (e.g. some
     * ProtonMail or Steam entries) have always been valid and were loadable before
     * 5.1.0 added validation; rejecting them locked users out of the whole database
     * (see issue #458). Require at least one of label/issuer to be non-empty. */
    gboolean has_label  = (label  != NULL && label[0]  != '\0');
    gboolean has_issuer = (issuer != NULL && issuer[0] != '\0');
    if (!has_label && !has_issuer) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Token %" G_GSIZE_FORMAT " has neither a label nor an issuer.", index);
        return FALSE;
    }
    if (!otp_secret_is_valid_base32 (secret)) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Token %" G_GSIZE_FORMAT " has a missing or invalid Base32 secret.", index);
        return FALSE;
    }
    if (!is_supported_algo (algo)) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Token %" G_GSIZE_FORMAT " has an unsupported or missing algorithm.", index);
        return FALSE;
    }
    if (!json_is_integer (digits_obj)) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Token %" G_GSIZE_FORMAT " has a missing digits field.", index);
        return FALSE;
    }
    json_int_t digits = json_integer_value (digits_obj);
    if (digits < 6 || digits > 8) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Token %" G_GSIZE_FORMAT " has an out-of-range digits value.", index);
        return FALSE;
    }

    json_t *group = json_object_get (obj, "group");
    if (group != NULL && !json_is_string (group)) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Token %" G_GSIZE_FORMAT " has a non-string group.", index);
        return FALSE;
    }

    if (g_ascii_strcasecmp (type, "TOTP") == 0) {
        json_t *period_obj = json_object_get (obj, "period");
        if (!json_is_integer (period_obj)) {
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                         "Token %" G_GSIZE_FORMAT " has a missing TOTP period.", index);
            return FALSE;
        }
        json_int_t period = json_integer_value (period_obj);
        if (period <= 0 || period > 300) {
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                         "Token %" G_GSIZE_FORMAT " has an out-of-range TOTP period.", index);
            return FALSE;
        }
    } else {
        json_t *counter_obj = json_object_get (obj, "counter");
        if (!json_is_integer (counter_obj)) {
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                         "Token %" G_GSIZE_FORMAT " has a missing HOTP counter.", index);
            return FALSE;
        }
        json_int_t counter = json_integer_value (counter_obj);
        if (counter < 0 || (guint64) counter >= OTP_HOTP_COUNTER_MAX) {
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                         "Token %" G_GSIZE_FORMAT " has an out-of-range HOTP counter.", index);
            return FALSE;
        }
    }

    return TRUE;
}

gboolean
otp_validate_database_root (json_t  *root,
                            GError **err)
{
    if (!json_is_array (root)) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Database JSON root must be an array.");
        return FALSE;
    }

    gsize index;
    json_t *obj;
    json_array_foreach (root, index, obj) {
        if (!otp_validate_token_object (obj, index, err))
            return FALSE;
    }
    return TRUE;
}

gboolean
otp_validate_import_token (const otp_t  *otp,
                           GError      **err)
{
    if (otp == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Import token is NULL.");
        return FALSE;
    }
    if (otp->type == NULL || (g_ascii_strcasecmp (otp->type, "TOTP") != 0 &&
                              g_ascii_strcasecmp (otp->type, "HOTP") != 0)) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Import token has an unsupported or missing type.");
        return FALSE;
    }
    /* Mirror otp_validate_token_object: an issuer-only token is valid. */
    gboolean has_label  = (otp->account_name != NULL && otp->account_name[0] != '\0');
    gboolean has_issuer = (otp->issuer       != NULL && otp->issuer[0]       != '\0');
    if (!has_label && !has_issuer) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Import token has neither an account label nor an issuer.");
        return FALSE;
    }
    if (!otp_secret_is_valid_base32 (otp->secret)) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Import token has a missing or invalid Base32 secret.");
        return FALSE;
    }
    if (!is_supported_algo (otp->algo)) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Import token has an unsupported or missing algorithm.");
        return FALSE;
    }
    if (otp->digits < 6 || otp->digits > 8) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Import token has an out-of-range digits value.");
        return FALSE;
    }
    if (g_ascii_strcasecmp (otp->type, "TOTP") == 0) {
        if (otp->period == 0 || otp->period > 300) {
            g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                         "Import token has an out-of-range TOTP period.");
            return FALSE;
        }
    } else if (otp->counter >= OTP_HOTP_COUNTER_MAX) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "Import token has an out-of-range HOTP counter.");
        return FALSE;
    }
    return TRUE;
}

gboolean
otp_repair_anonymous_token_object (json_t *obj,
                                   gsize   index)
{
    if (!json_is_object (obj))
        return FALSE;

    const gchar *label  = json_string_value (json_object_get (obj, "label"));
    const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
    gboolean has_label  = (label  != NULL && label[0]  != '\0');
    gboolean has_issuer = (issuer != NULL && issuer[0] != '\0');
    if (has_label || has_issuer)
        return FALSE;

    gchar *placeholder = otp_anonymous_placeholder (index);
    json_object_set_new (obj, "label", json_string (placeholder));
    g_free (placeholder);
    return TRUE;
}

guint
otp_repair_database_root (json_t *root)
{
    if (!json_is_array (root))
        return 0;

    guint repaired = 0;
    gsize index;
    json_t *obj;
    json_array_foreach (root, index, obj) {
        if (otp_repair_anonymous_token_object (obj, index))
            repaired++;
    }
    return repaired;
}

gboolean
otp_repair_anonymous_import_token (otp_t *otp,
                                   gsize  index)
{
    if (otp == NULL)
        return FALSE;

    gboolean has_label  = (otp->account_name != NULL && otp->account_name[0] != '\0');
    gboolean has_issuer = (otp->issuer       != NULL && otp->issuer[0]       != '\0');
    if (has_label || has_issuer)
        return FALSE;

    /* account_name is non-sensitive and freed with g_free across the importers,
     * so g_strdup keeps the allocator contract (even while Jansson's global
     * allocator is gcry secure memory). */
    g_free (otp->account_name);
    otp->account_name = otp_anonymous_placeholder (index);
    return TRUE;
}
