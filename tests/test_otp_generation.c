#include <glib.h>
#include <cotp.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* RFC 6238 Appendix B + RFC 4226 Appendix D reference test vectors.
 *
 * The canonical secret for SHA1 is the ASCII string "12345678901234567890"
 * (20 bytes). RFC 6238 extends this for SHA256/SHA512 by repeating "12345..."
 * up to 32 / 64 bytes total. The shared secrets are passed to the HMAC as
 * raw bytes; cotp's API takes them base32-encoded, so each test computes
 * the encoding at runtime via base32_encode() rather than hard-coding it.
 *
 * Pinning the output to the RFC anchors otpclient's user-visible behaviour:
 * any regression in cotp, in the build flags, or in otpclient's secret
 * handling around cotp will trip these tests immediately. */

static const char SECRET_SHA1_ASCII[]   = "12345678901234567890";
static const char SECRET_SHA256_ASCII[] = "12345678901234567890123456789012";
static const char SECRET_SHA512_ASCII[] = "1234567890123456789012345678901234567890123456789012345678901234";

typedef struct {
    long time;
    const gchar *expected;
} totp_vector_t;

static gchar *
ascii_to_base32 (const char *ascii)
{
    cotp_error_t err = NO_ERROR;
    gchar *b32 = base32_encode ((const uint8_t *) ascii, strlen (ascii), &err);
    g_assert_cmpint (err, ==, NO_ERROR);
    g_assert_nonnull (b32);
    return b32;
}

static void
assert_totp_vectors (const char          *ascii_secret,
                     int                  algo,
                     const totp_vector_t *vectors,
                     gsize                count)
{
    gchar *b32 = ascii_to_base32 (ascii_secret);
    for (gsize i = 0; i < count; i++) {
        cotp_error_t err = NO_ERROR;
        gchar *otp = get_totp_at (b32, vectors[i].time, 8, 30, algo, &err);
        g_assert_cmpint (err, ==, NO_ERROR);
        g_assert_nonnull (otp);
        g_assert_cmpstr (otp, ==, vectors[i].expected);
        free (otp);
    }
    free (b32);
}

static void
test_rfc6238_sha1_vectors (void)
{
    static const totp_vector_t vectors[] = {
        {          59L, "94287082" },
        {  1111111109L, "07081804" },
        {  1111111111L, "14050471" },
        {  1234567890L, "89005924" },
        {  2000000000L, "69279037" },
        { 20000000000L, "65353130" },
    };
    assert_totp_vectors (SECRET_SHA1_ASCII, COTP_SHA1, vectors, G_N_ELEMENTS (vectors));
}

static void
test_rfc6238_sha256_vectors (void)
{
    static const totp_vector_t vectors[] = {
        {          59L, "46119246" },
        {  1111111109L, "68084774" },
        {  1111111111L, "67062674" },
        {  1234567890L, "91819424" },
        {  2000000000L, "90698825" },
        { 20000000000L, "77737706" },
    };
    assert_totp_vectors (SECRET_SHA256_ASCII, COTP_SHA256, vectors, G_N_ELEMENTS (vectors));
}

static void
test_rfc6238_sha512_vectors (void)
{
    static const totp_vector_t vectors[] = {
        {          59L, "90693936" },
        {  1111111109L, "25091201" },
        {  1111111111L, "99943326" },
        {  1234567890L, "93441116" },
        {  2000000000L, "38618901" },
        { 20000000000L, "47863826" },
    };
    assert_totp_vectors (SECRET_SHA512_ASCII, COTP_SHA512, vectors, G_N_ELEMENTS (vectors));
}

static void
test_rfc4226_hotp_vectors (void)
{
    static const gchar *expected[] = {
        "755224", "287082", "359152", "969429", "338314",
        "254676", "287922", "162583", "399871", "520489",
    };
    g_autofree gchar *b32 = ascii_to_base32 (SECRET_SHA1_ASCII);
    for (long counter = 0; counter < (long) G_N_ELEMENTS (expected); counter++) {
        cotp_error_t err = NO_ERROR;
        gchar *otp = get_hotp (b32, counter, 6, COTP_SHA1, &err);
        g_assert_cmpint (err, ==, NO_ERROR);
        g_assert_nonnull (otp);
        g_assert_cmpstr (otp, ==, expected[counter]);
        free (otp);
    }
}

static void
test_totp_zero_pads_short_value (void)
{
    /* The 8-digit form of RFC 6238 SHA1 at T=1111111109 is "07081804" - the
     * leading zero must be preserved by zero-padding rather than dropped. */
    g_autofree gchar *b32 = ascii_to_base32 (SECRET_SHA1_ASCII);
    cotp_error_t err = NO_ERROR;
    gchar *otp = get_totp_at (b32, 1111111109L, 8, 30, COTP_SHA1, &err);
    g_assert_cmpint (err, ==, NO_ERROR);
    g_assert_nonnull (otp);
    g_assert_cmpuint (strlen (otp), ==, 8);
    g_assert_cmpint (otp[0], ==, '0');
    free (otp);
}

static void
test_invalid_base32_secret_sets_err (void)
{
    cotp_error_t err = NO_ERROR;
    gchar *otp = get_totp_at ("not-base-32!", 59L, 6, 30, COTP_SHA1, &err);
    g_assert_null (otp);
    g_assert_cmpint (err, ==, INVALID_B32_INPUT);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/otp-generation/rfc6238-sha1",   test_rfc6238_sha1_vectors);
    g_test_add_func ("/otp-generation/rfc6238-sha256", test_rfc6238_sha256_vectors);
    g_test_add_func ("/otp-generation/rfc6238-sha512", test_rfc6238_sha512_vectors);
    g_test_add_func ("/otp-generation/rfc4226-hotp",   test_rfc4226_hotp_vectors);
    g_test_add_func ("/otp-generation/zero-pad",       test_totp_zero_pads_short_value);
    g_test_add_func ("/otp-generation/invalid-b32",    test_invalid_base32_secret_sets_err);

    return g_test_run ();
}
