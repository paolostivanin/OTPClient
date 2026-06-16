#include <glib.h>
#include <gcrypt.h>
#include "common.h"
#include "parse-uri.h"

static void
test_valid_uri (void)
{
    GSList *otps = NULL;
    set_otps_from_uris ("otpauth://totp/Example:alice?secret=JBSWY3DPEHPK3PXP&issuer=Example&period=30&digits=6", &otps);

    g_assert_nonnull (otps);
    g_assert_null (otps->next);

    otp_t *otp = otps->data;
    g_assert_cmpstr (otp->type, ==, "TOTP");
    g_assert_cmpstr (otp->account_name, ==, "alice");
    g_assert_cmpstr (otp->issuer, ==, "Example");
    g_assert_cmpstr (otp->secret, ==, "JBSWY3DPEHPK3PXP");
    g_assert_cmpuint (otp->period, ==, 30);

    free_otps_gslist (otps, 0);
}

static void
test_missing_secret_rejected (void)
{
    GSList *otps = NULL;
    set_otps_from_uris ("otpauth://totp/Example:alice?issuer=Example", &otps);
    g_assert_null (otps);
}

static void
test_invalid_escape_rejected (void)
{
    GSList *otps = NULL;
    set_otps_from_uris ("otpauth://totp/Example%ZZalice?secret=JBSWY3DPEHPK3PXP", &otps);
    g_assert_null (otps);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    gchar *init_err = init_libs (1024 * 1024);
    g_assert_null (init_err);

    g_test_add_func ("/parse-uri/valid", test_valid_uri);
    g_test_add_func ("/parse-uri/missing-secret", test_missing_secret_rejected);
    g_test_add_func ("/parse-uri/invalid-escape", test_invalid_escape_rejected);

    return g_test_run ();
}
