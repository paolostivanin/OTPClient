#define _DEFAULT_SOURCE
#include <glib.h>
#include <gcrypt.h>
#include "common.h"
#include "google-migration.h"
#include "google-migration.pb-c.h"
#include "gquarks.h"

static gchar *
pack_uri (MigrationPayload *payload)
{
    gsize size = migration_payload__get_packed_size (payload);
    guchar *bytes = g_malloc (size);
    migration_payload__pack (payload, bytes);
    g_autofree gchar *base64 = g_base64_encode (bytes, size);
    explicit_bzero (bytes, size);
    g_free (bytes);
    g_autofree gchar *escaped = g_uri_escape_string (base64, NULL, FALSE);
    return g_strdup_printf ("otpauth-migration://offline?data=%s", escaped);
}

static void
test_valid_payload (void)
{
    guint8 secret[] = { 0x48, 0x65, 0x6c, 0x6c, 0x6f };
    gchar name[] = "Example:alice";
    gchar issuer[] = "Example";
    MigrationPayload__OtpParameters parameter =
        MIGRATION_PAYLOAD__OTP_PARAMETERS__INIT;
    parameter.secret.data = secret;
    parameter.secret.len = sizeof secret;
    parameter.name = name;
    parameter.issuer = issuer;
    parameter.algorithm = MIGRATION_PAYLOAD__ALGORITHM__ALGORITHM_SHA1;
    parameter.digits = MIGRATION_PAYLOAD__DIGIT_COUNT__DIGIT_COUNT_SIX;
    parameter.type = MIGRATION_PAYLOAD__OTP_TYPE__OTP_TYPE_TOTP;

    MigrationPayload payload = MIGRATION_PAYLOAD__INIT;
    MigrationPayload__OtpParameters *parameters[] = { &parameter };
    payload.n_otp_parameters = 1;
    payload.otp_parameters = parameters;
    payload.batch_size = 2;
    payload.batch_index = 0;

    g_autofree gchar *uri = pack_uri (&payload);
    guint invalid = 0, batch_size = 0, batch_index = 0;
    GError *err = NULL;
    GSList *otps = google_migration_decode (uri, &invalid, &batch_size,
                                            &batch_index, &err);
    g_assert_no_error (err);
    g_assert_nonnull (otps);
    g_assert_cmpuint (invalid, ==, 0);
    g_assert_cmpuint (batch_size, ==, 2);
    otp_t *otp = otps->data;
    g_assert_cmpstr (otp->account_name, ==, "alice");
    g_assert_cmpstr (otp->issuer, ==, "Example");
    g_assert_cmpstr (otp->secret, ==, "JBSWY3DP");
    free_otps_gslist (otps, 1);
}

static void
test_malformed_payload (void)
{
    GError *err = NULL;
    GSList *otps = google_migration_decode (
        "otpauth-migration://offline?data=not-protobuf",
        NULL, NULL, NULL, &err);
    g_assert_null (otps);
    g_assert_error (err, generic_error_gquark (), GENERIC_ERRCODE);
    g_clear_error (&err);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    gchar *init_err = init_libs (DEFAULT_MEMLOCK_VALUE);
    g_assert_null (init_err);
    g_test_add_func ("/google-migration/valid", test_valid_payload);
    g_test_add_func ("/google-migration/malformed", test_malformed_payload);
    return g_test_run ();
}
