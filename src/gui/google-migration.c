#define _DEFAULT_SOURCE
#include <cotp.h>
#include <gcrypt.h>
#include <string.h>
#include "google-migration.h"
#include "google-migration.pb-c.h"
#include "gquarks.h"
#include "otp-validation.h"

#define MIGRATION_PREFIX "otpauth-migration://offline?"
#define MAX_MIGRATION_PAYLOAD (1024u * 1024u)
#define MAX_MIGRATION_TOKENS 100u
#define MAX_MIGRATION_BATCHES 100u

static void
free_otp (otp_t *otp)
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

GSList *
google_migration_decode (const gchar  *uri,
                         guint        *invalid_count,
                         guint        *batch_size,
                         guint        *batch_index,
                         GError      **error)
{
    g_return_val_if_fail (error == NULL || *error == NULL, NULL);
    if (invalid_count != NULL)
        *invalid_count = 0;
    if (batch_size != NULL)
        *batch_size = 0;
    if (batch_index != NULL)
        *batch_index = 0;

    if (uri == NULL || !g_str_has_prefix (uri, MIGRATION_PREFIX)) {
        g_set_error (error, generic_error_gquark (), GENERIC_ERRCODE,
                     "QR code is not a Google migration payload.");
        return NULL;
    }

    g_autoptr (GError) uri_error = NULL;
    g_autoptr (GUri) parsed = g_uri_parse (uri, G_URI_FLAGS_NONE, &uri_error);
    if (parsed == NULL) {
        g_set_error (error, generic_error_gquark (), GENERIC_ERRCODE,
                     "Malformed Google migration URI.");
        return NULL;
    }
    const gchar *query = g_uri_get_query (parsed);
    g_autoptr (GHashTable) params = query != NULL
        ? g_uri_parse_params (query, -1, "&", G_URI_PARAMS_NONE, &uri_error)
        : NULL;
    const gchar *encoded = params != NULL ? g_hash_table_lookup (params, "data") : NULL;
    if (encoded == NULL || encoded[0] == '\0') {
        g_set_error (error, generic_error_gquark (), GENERIC_ERRCODE,
                     "Google migration payload has no data field.");
        return NULL;
    }

    gsize raw_len = 0;
    guchar *raw = g_base64_decode (encoded, &raw_len);
    if (raw == NULL || raw_len == 0 || raw_len > MAX_MIGRATION_PAYLOAD) {
        g_free (raw);
        g_set_error (error, generic_error_gquark (), GENERIC_ERRCODE,
                     "Google migration payload size is invalid.");
        return NULL;
    }

    MigrationPayload *payload = migration_payload__unpack (NULL, raw_len, raw);
    explicit_bzero (raw, raw_len);
    g_free (raw);
    if (payload == NULL || payload->n_otp_parameters > MAX_MIGRATION_TOKENS) {
        if (payload != NULL)
            migration_payload__free_unpacked (payload, NULL);
        g_set_error (error, generic_error_gquark (), GENERIC_ERRCODE,
                     "Google migration payload is malformed or contains too many tokens.");
        return NULL;
    }
    if (payload->batch_size <= 0 ||
        payload->batch_size > (gint32) MAX_MIGRATION_BATCHES ||
        payload->batch_index < 0 ||
        payload->batch_index >= payload->batch_size) {
        migration_payload__free_unpacked (payload, NULL);
        g_set_error (error, generic_error_gquark (), GENERIC_ERRCODE,
                     "Google migration batch metadata is invalid.");
        return NULL;
    }

    if (batch_size != NULL)
        *batch_size = (guint) payload->batch_size;
    if (batch_index != NULL)
        *batch_index = (guint) payload->batch_index;

    GSList *result = NULL;
    guint invalid = 0;
    for (gsize i = 0; i < payload->n_otp_parameters; i++) {
        MigrationPayload__OtpParameters *src = payload->otp_parameters[i];
        if (src == NULL || src->secret.data == NULL || src->secret.len == 0 ||
            src->name == NULL || src->name[0] == '\0') {
            invalid++;
            continue;
        }

        otp_t *otp = g_new0 (otp_t, 1);
        otp->issuer = g_strdup (src->issuer != NULL ? src->issuer : "");
        const gchar *account = src->name;
        const gchar *colon = strchr (src->name, ':');
        if (colon != NULL && colon[1] != '\0')
            account = colon + 1;
        otp->account_name = g_strdup (account);

        switch (src->type) {
            case MIGRATION_PAYLOAD__OTP_TYPE__OTP_TYPE_TOTP:
                otp->type = g_strdup ("TOTP");
                otp->period = 30;
                break;
            case MIGRATION_PAYLOAD__OTP_TYPE__OTP_TYPE_HOTP:
                otp->type = g_strdup ("HOTP");
                otp->counter = src->counter >= 0 ? (guint64) src->counter
                                                  : OTP_HOTP_COUNTER_MAX;
                break;
            default:
                free_otp (otp);
                invalid++;
                continue;
        }

        switch (src->algorithm) {
            case MIGRATION_PAYLOAD__ALGORITHM__ALGORITHM_UNSPECIFIED:
            case MIGRATION_PAYLOAD__ALGORITHM__ALGORITHM_SHA1:
                otp->algo = g_strdup ("SHA1");
                break;
            case MIGRATION_PAYLOAD__ALGORITHM__ALGORITHM_SHA256:
                otp->algo = g_strdup ("SHA256");
                break;
            case MIGRATION_PAYLOAD__ALGORITHM__ALGORITHM_SHA512:
                otp->algo = g_strdup ("SHA512");
                break;
            default:
                free_otp (otp);
                invalid++;
                continue;
        }

        otp->digits = src->digits == MIGRATION_PAYLOAD__DIGIT_COUNT__DIGIT_COUNT_EIGHT
            ? 8 : 6;
        cotp_error_t encode_error = NO_ERROR;
        gchar *base32 = base32_encode (src->secret.data, src->secret.len,
                                       &encode_error);
        if (base32 == NULL || encode_error != NO_ERROR) {
            sensitive_free (base32);
            free_otp (otp);
            invalid++;
            continue;
        }
        otp->secret = secure_strdup (base32);
        sensitive_free (base32);

        GError *validation_error = NULL;
        if (otp->secret == NULL ||
            !otp_validate_import_token (otp, &validation_error)) {
            g_clear_error (&validation_error);
            free_otp (otp);
            invalid++;
            continue;
        }
        result = g_slist_append (result, otp);
    }

    migration_payload__free_unpacked (payload, NULL);
    if (invalid_count != NULL)
        *invalid_count = invalid;
    if (result == NULL) {
        g_set_error (error, generic_error_gquark (), GENERIC_ERRCODE,
                     "Google migration payload contains no valid tokens.");
    }
    return result;
}
