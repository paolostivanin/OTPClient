#include <glib.h>
#include <jansson.h>
#include "common.h"
#include "otp-validation.h"
#include "parse-uri.h"

static otp_t *
parse_single (const gchar *uri)
{
    GSList *otps = NULL;
    set_otps_from_uris (uri, &otps);
    if (otps == NULL)
        return NULL;
    g_assert_null (otps->next);
    otp_t *otp = otps->data;
    g_slist_free (otps);
    return otp;
}

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

static void
test_hotp_uri_parsed (void)
{
    otp_t *otp = parse_single (
        "otpauth://hotp/Example:alice?secret=JBSWY3DPEHPK3PXP&issuer=Example&counter=42&digits=6");
    g_assert_nonnull (otp);
    g_assert_cmpstr (otp->type, ==, "HOTP");
    g_assert_cmpstr (otp->account_name, ==, "alice");
    g_assert_cmpuint ((guint64) otp->counter, ==, 42);
    g_assert_cmpuint (otp->digits, ==, 6);
    free_otp (otp);
}

static void
test_sha256_algorithm (void)
{
    otp_t *otp = parse_single (
        "otpauth://totp/Example:alice?secret=JBSWY3DPEHPK3PXP&issuer=Example&algorithm=SHA256");
    g_assert_nonnull (otp);
    g_assert_cmpstr (otp->algo, ==, "SHA256");
    free_otp (otp);
}

static void
test_sha512_algorithm (void)
{
    otp_t *otp = parse_single (
        "otpauth://totp/Example:alice?secret=JBSWY3DPEHPK3PXP&issuer=Example&algorithm=SHA512");
    g_assert_nonnull (otp);
    g_assert_cmpstr (otp->algo, ==, "SHA512");
    free_otp (otp);
}

static void
test_unknown_algorithm_falls_back_to_sha1 (void)
{
    /* parse_uri only overrides the default ("SHA1") when the algorithm string
     * is one of the three supported. Unknown values silently keep SHA1, which
     * downstream validation accepts. This pins that fallback behaviour. */
    otp_t *otp = parse_single (
        "otpauth://totp/Example:alice?secret=JBSWY3DPEHPK3PXP&issuer=Example&algorithm=MD5");
    g_assert_nonnull (otp);
    g_assert_cmpstr (otp->algo, ==, "SHA1");
    free_otp (otp);
}

static void
test_period_out_of_bounds (void)
{
    /* period=0 and period=999 fall outside [1,120]; parse_uri keeps the
     * default of 30 in those cases. The resulting otp_t is still valid. */
    otp_t *zero = parse_single (
        "otpauth://totp/Example:alice?secret=JBSWY3DPEHPK3PXP&issuer=Example&period=0");
    g_assert_nonnull (zero);
    g_assert_cmpuint (zero->period, ==, 30);
    free_otp (zero);

    otp_t *huge = parse_single (
        "otpauth://totp/Example:alice?secret=JBSWY3DPEHPK3PXP&issuer=Example&period=999");
    g_assert_nonnull (huge);
    g_assert_cmpuint (huge->period, ==, 30);
    free_otp (huge);
}

static void
test_digits_out_of_bounds (void)
{
    otp_t *small = parse_single (
        "otpauth://totp/Example:alice?secret=JBSWY3DPEHPK3PXP&issuer=Example&digits=3");
    g_assert_nonnull (small);
    g_assert_cmpuint (small->digits, ==, 6);
    free_otp (small);

    otp_t *big = parse_single (
        "otpauth://totp/Example:alice?secret=JBSWY3DPEHPK3PXP&issuer=Example&digits=11");
    g_assert_nonnull (big);
    g_assert_cmpuint (big->digits, ==, 6);
    free_otp (big);
}

static void
test_digits_widened_range (void)
{
    /* digits 4-10 is libcotp's real range (issue #464). Values the old 6-8
     * clamp used to reset to 6 - 4, 5, 9, 10 - must now be preserved. */
    const guint kept[] = { 4, 5, 9, 10 };
    for (guint i = 0; i < G_N_ELEMENTS (kept); i++) {
        g_autofree gchar *uri = g_strdup_printf (
            "otpauth://totp/Example:alice?secret=JBSWY3DPEHPK3PXP&issuer=Example&digits=%u",
            kept[i]);
        otp_t *otp = parse_single (uri);
        g_assert_nonnull (otp);
        g_assert_cmpuint (otp->digits, ==, kept[i]);
        free_otp (otp);
    }
}

static void
test_period_upper_bound (void)
{
    /* 120 is the top of libcotp's range and must be kept; 150 is above it and
     * falls back to the default 30 (issue #464). */
    otp_t *at_max = parse_single (
        "otpauth://totp/Example:alice?secret=JBSWY3DPEHPK3PXP&issuer=Example&period=120");
    g_assert_nonnull (at_max);
    g_assert_cmpuint (at_max->period, ==, 120);
    free_otp (at_max);

    otp_t *over = parse_single (
        "otpauth://totp/Example:alice?secret=JBSWY3DPEHPK3PXP&issuer=Example&period=150");
    g_assert_nonnull (over);
    g_assert_cmpuint (over->period, ==, 30);
    free_otp (over);
}

static void
test_counter_at_max (void)
{
    g_autofree gchar *at_max = g_strdup_printf (
        "otpauth://hotp/Example:alice?secret=JBSWY3DPEHPK3PXP&issuer=Example&counter=%" G_GUINT64_FORMAT,
        OTP_HOTP_COUNTER_MAX - 1);
    otp_t *otp = parse_single (at_max);
    g_assert_nonnull (otp);
    g_assert_cmpuint ((guint64) otp->counter, ==, OTP_HOTP_COUNTER_MAX - 1);
    free_otp (otp);

    /* MAX+1: parse_uri silently keeps counter=0 (the default). The import
     * token still validates because counter==0 is in range; this asserts
     * that the out-of-bounds value never lands in the struct. */
    g_autofree gchar *over_max = g_strdup_printf (
        "otpauth://hotp/Example:alice?secret=JBSWY3DPEHPK3PXP&issuer=Example&counter=%" G_GUINT64_FORMAT,
        OTP_HOTP_COUNTER_MAX);
    otp_t *over = parse_single (over_max);
    g_assert_nonnull (over);
    g_assert_cmpuint ((guint64) over->counter, ==, 0);
    free_otp (over);
}

static void
test_malformed_scheme_ignored (void)
{
    GSList *otps = NULL;
    /* set_otps_from_uris splits on '\n' and only passes through tokens that
     * contain the substring "otpauth". A bare "notauth://" without that
     * substring is filtered out without entering parse_uri at all. */
    set_otps_from_uris ("notauth://totp/Example:alice?secret=JBSWY3DPEHPK3PXP", &otps);
    g_assert_null (otps);
}

static void
test_invalid_secret_rejected (void)
{
    /* base32 character set excludes "1"/"0" (it goes A-Z, 2-7). A secret of
     * "0000" therefore must fail validation in otp_validate_import_token
     * and never produce an otp_t. */
    GSList *otps = NULL;
    set_otps_from_uris ("otpauth://totp/Example:alice?secret=0000&issuer=Example", &otps);
    g_assert_null (otps);
}

static void
test_roundtrip_parse_emit_parse (void)
{
    /* Build a fully-specified token, emit it as a URI, parse the URI back,
     * and assert every user-visible field round-trips intact. This is the
     * only end-to-end pin on the encoder/decoder symmetry: if either side
     * loses a field, the assertions trip. */
    json_t *obj = build_json_obj ("TOTP", "alice", "Example",
                                  "JBSWY3DPEHPK3PXP", 8, "SHA256", 60, 0, NULL);
    g_autofree gchar *uri = get_otpauth_uri (obj);
    g_assert_cmpstr (uri, !=, "");

    otp_t *otp = parse_single (uri);
    g_assert_nonnull (otp);
    g_assert_cmpstr (otp->type, ==, "TOTP");
    g_assert_cmpstr (otp->account_name, ==, "alice");
    g_assert_cmpstr (otp->issuer, ==, "Example");
    g_assert_cmpstr (otp->secret, ==, "JBSWY3DPEHPK3PXP");
    g_assert_cmpstr (otp->algo, ==, "SHA256");
    g_assert_cmpuint (otp->digits, ==, 8);
    g_assert_cmpuint (otp->period, ==, 60);
    free_otp (otp);

    json_decref (obj);
}

static void
test_roundtrip_hotp_parse_emit_parse (void)
{
    json_t *obj = build_json_obj ("HOTP", "bob", "Acme",
                                  "JBSWY3DPEHPK3PXP", 6, "SHA1", 0, 7, NULL);
    g_autofree gchar *uri = get_otpauth_uri (obj);
    g_assert_cmpstr (uri, !=, "");

    otp_t *otp = parse_single (uri);
    g_assert_nonnull (otp);
    g_assert_cmpstr (otp->type, ==, "HOTP");
    g_assert_cmpstr (otp->account_name, ==, "bob");
    g_assert_cmpstr (otp->issuer, ==, "Acme");
    g_assert_cmpuint ((guint64) otp->counter, ==, 7);
    free_otp (otp);

    json_decref (obj);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    gchar *init_err = init_libs (1024 * 1024);
    g_assert_null (init_err);

    g_test_add_func ("/parse-uri-extra/hotp",                  test_hotp_uri_parsed);
    g_test_add_func ("/parse-uri-extra/sha256",                test_sha256_algorithm);
    g_test_add_func ("/parse-uri-extra/sha512",                test_sha512_algorithm);
    g_test_add_func ("/parse-uri-extra/unknown-algorithm",     test_unknown_algorithm_falls_back_to_sha1);
    g_test_add_func ("/parse-uri-extra/period-out-of-bounds",  test_period_out_of_bounds);
    g_test_add_func ("/parse-uri-extra/digits-out-of-bounds",  test_digits_out_of_bounds);
    g_test_add_func ("/parse-uri-extra/digits-widened-range",  test_digits_widened_range);
    g_test_add_func ("/parse-uri-extra/period-upper-bound",    test_period_upper_bound);
    g_test_add_func ("/parse-uri-extra/counter-at-max",        test_counter_at_max);
    g_test_add_func ("/parse-uri-extra/malformed-scheme",      test_malformed_scheme_ignored);
    g_test_add_func ("/parse-uri-extra/invalid-secret",        test_invalid_secret_rejected);
    g_test_add_func ("/parse-uri-extra/roundtrip-totp",        test_roundtrip_parse_emit_parse);
    g_test_add_func ("/parse-uri-extra/roundtrip-hotp",        test_roundtrip_hotp_parse_emit_parse);

    return g_test_run ();
}
