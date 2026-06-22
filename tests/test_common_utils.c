#include <glib.h>
#include <jansson.h>
#include "common.h"
#include "gquarks.h"
#include "otp-validation.h"

/* ---------- hexstr_to_bytes / bytes_to_hexstr / secure_strdup ---------- */

static void
test_hexstr_to_bytes_valid (void)
{
    /* Sanity-check the success path so the failure tests aren't
     * accidentally trivially passing because the helper is broken. */
    guchar *out = hexstr_to_bytes ("0a1bff");
    g_assert_nonnull (out);
    g_assert_cmpuint (out[0], ==, 0x0a);
    g_assert_cmpuint (out[1], ==, 0x1b);
    g_assert_cmpuint (out[2], ==, 0xff);
    g_free (out);
}

static void
test_hexstr_to_bytes_odd_length (void)
{
    g_assert_null (hexstr_to_bytes ("abc"));
}

static void
test_hexstr_to_bytes_non_hex_chars (void)
{
    /* 'G' and 'Z' are outside [0-9a-fA-F]; the helper must reject the
     * whole string rather than silently encode garbage. */
    g_assert_null (hexstr_to_bytes ("GG"));
    g_assert_null (hexstr_to_bytes ("0Z"));
}

static void
test_hexstr_to_bytes_null (void)
{
    g_assert_null (hexstr_to_bytes (NULL));
}

static void
test_hexstr_to_bytes_empty (void)
{
    g_assert_null (hexstr_to_bytes (""));
}

static void
test_hexstr_to_bytes_exact_wrong_length (void)
{
    /* Caller asked for 4 bytes but supplied 6 hex chars (= 3 bytes); the
     * exact variant must refuse rather than truncate. */
    g_assert_null (hexstr_to_bytes_exact ("0a1bff", 4));
}

static void
test_bytes_to_hexstr_null_data (void)
{
    /* NULL data with non-zero length must return NULL rather than
     * dereferencing. NULL+zero-length is a legitimate "empty" request and
     * returns an empty string. */
    g_assert_null (bytes_to_hexstr (NULL, 4));

    gchar *empty = bytes_to_hexstr (NULL, 0);
    g_assert_nonnull (empty);
    g_assert_cmpstr (empty, ==, "");
    g_free (empty);
}

static void
test_bytes_to_hexstr_roundtrip (void)
{
    const guchar in[] = { 0x00, 0x12, 0xab, 0xff };
    gchar *hex = bytes_to_hexstr (in, sizeof in);
    g_assert_nonnull (hex);
    g_assert_cmpstr (hex, ==, "0012abff");

    guchar *back = hexstr_to_bytes (hex);
    g_assert_nonnull (back);
    for (gsize i = 0; i < sizeof in; i++)
        g_assert_cmpuint (back[i], ==, in[i]);

    g_free (hex);
    g_free (back);
}

static void
test_bytes_to_hexstr_overflow (void)
{
    const guchar byte = 0;
    g_assert_null (bytes_to_hexstr (&byte, (G_MAXSIZE / 2) + 1));
}

static void
test_secure_strdup_null (void)
{
    g_assert_null (secure_strdup (NULL));
}

static void
test_secure_strdup_empty (void)
{
    gchar *s = secure_strdup ("");
    g_assert_nonnull (s);
    g_assert_cmpuint (strlen (s), ==, 0);
    gcry_free (s);
}

static void
test_get_algo_int_from_str_known (void)
{
    /* The three supported algorithms must map to the exact COTP_SHA* values
     * declared in cotp.h. These mappings feed straight into get_totp_at()
     * and a regression here would silently break OTP generation. */
    g_assert_cmpint (get_algo_int_from_str ("SHA1"),   ==, 0);
    g_assert_cmpint (get_algo_int_from_str ("SHA256"), ==, 1);
    g_assert_cmpint (get_algo_int_from_str ("SHA512"), ==, 2);
}

static void
test_get_algo_int_from_str_unknown (void)
{
    /* Unknown algorithm strings fall back to SHA1 and emit a g_warning.
     * We expect both the warning AND the fallback value: the warning is
     * the user-visible signal that something is off, the fallback keeps
     * the app usable. */
    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING,
                           "Unknown OTP algorithm 'MD5'*");
    g_assert_cmpint (get_algo_int_from_str ("MD5"), ==, 0);
    g_test_assert_expected_messages ();

    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING,
                           "Unknown OTP algorithm '(null)'*");
    g_assert_cmpint (get_algo_int_from_str (NULL), ==, 0);
    g_test_assert_expected_messages ();
}

/* ---------- otp-validation field gaps ---------- */

static json_t *
valid_totp_obj (void)
{
    return build_json_obj ("TOTP", "alice", "Example", "JBSWY3DPEHPK3PXP",
                           6, "SHA1", 30, 0, NULL);
}

static void
assert_root_rejected (json_t *obj_to_wrap)
{
    json_t *root = json_array ();
    json_array_append_new (root, obj_to_wrap);

    GError *err = NULL;
    g_assert_false (otp_validate_database_root (root, &err));
    g_assert_error (err, generic_error_gquark (), GENERIC_ERRCODE);
    g_clear_error (&err);
    json_decref (root);
}

static void
test_validation_missing_type (void)
{
    json_t *obj = valid_totp_obj ();
    json_object_del (obj, "type");
    assert_root_rejected (obj);
}

static void
test_validation_missing_label (void)
{
    json_t *obj = valid_totp_obj ();
    json_object_del (obj, "label");
    assert_root_rejected (obj);
}

static void
test_validation_empty_label (void)
{
    json_t *obj = valid_totp_obj ();
    json_object_set_new (obj, "label", json_string (""));
    assert_root_rejected (obj);
}

static void
test_validation_missing_secret (void)
{
    json_t *obj = valid_totp_obj ();
    json_object_del (obj, "secret");
    assert_root_rejected (obj);
}

static void
test_validation_invalid_secret_base32 (void)
{
    json_t *obj = valid_totp_obj ();
    json_object_set_new (obj, "secret", json_string ("0000")); /* not base32 */
    assert_root_rejected (obj);
}

static void
test_validation_unknown_algorithm (void)
{
    json_t *obj = valid_totp_obj ();
    json_object_set_new (obj, "algo", json_string ("MD5"));
    assert_root_rejected (obj);
}

static void
test_validation_digits_below_range (void)
{
    json_t *obj = valid_totp_obj ();
    json_object_set_new (obj, "digits", json_integer (3));
    assert_root_rejected (obj);
}

static void
test_validation_digits_above_range (void)
{
    json_t *obj = valid_totp_obj ();
    json_object_set_new (obj, "digits", json_integer (11));
    assert_root_rejected (obj);
}

static void
test_validation_negative_period (void)
{
    json_t *obj = valid_totp_obj ();
    json_object_set_new (obj, "period", json_integer (-1));
    assert_root_rejected (obj);
}

static void
test_validation_period_above_range (void)
{
    json_t *obj = valid_totp_obj ();
    json_object_set_new (obj, "period", json_integer (301));
    assert_root_rejected (obj);
}

static void
test_validation_negative_hotp_counter (void)
{
    json_t *obj = build_json_obj ("HOTP", "alice", "Example", "JBSWY3DPEHPK3PXP",
                                  6, "SHA1", 30, 0, NULL);
    json_object_set_new (obj, "counter", json_integer (-1));
    assert_root_rejected (obj);
}

static void
test_validation_non_string_group (void)
{
    json_t *obj = valid_totp_obj ();
    json_object_set_new (obj, "group", json_integer (42));
    assert_root_rejected (obj);
}

static void
test_validation_unsupported_type (void)
{
    json_t *obj = valid_totp_obj ();
    json_object_set_new (obj, "type", json_string ("Steam"));
    assert_root_rejected (obj);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    gchar *init_err = init_libs (DEFAULT_MEMLOCK_VALUE);
    g_assert_null (init_err);

    g_test_add_func ("/common-utils/hexstr-to-bytes/valid",          test_hexstr_to_bytes_valid);
    g_test_add_func ("/common-utils/hexstr-to-bytes/odd-length",     test_hexstr_to_bytes_odd_length);
    g_test_add_func ("/common-utils/hexstr-to-bytes/non-hex",        test_hexstr_to_bytes_non_hex_chars);
    g_test_add_func ("/common-utils/hexstr-to-bytes/null",           test_hexstr_to_bytes_null);
    g_test_add_func ("/common-utils/hexstr-to-bytes/empty",          test_hexstr_to_bytes_empty);
    g_test_add_func ("/common-utils/hexstr-to-bytes-exact/wrong",    test_hexstr_to_bytes_exact_wrong_length);
    g_test_add_func ("/common-utils/bytes-to-hexstr/null",           test_bytes_to_hexstr_null_data);
    g_test_add_func ("/common-utils/bytes-to-hexstr/roundtrip",      test_bytes_to_hexstr_roundtrip);
    g_test_add_func ("/common-utils/bytes-to-hexstr/overflow",       test_bytes_to_hexstr_overflow);
    g_test_add_func ("/common-utils/secure-strdup/null",             test_secure_strdup_null);
    g_test_add_func ("/common-utils/secure-strdup/empty",            test_secure_strdup_empty);
    g_test_add_func ("/common-utils/get-algo-int-from-str/known",    test_get_algo_int_from_str_known);
    g_test_add_func ("/common-utils/get-algo-int-from-str/unknown",  test_get_algo_int_from_str_unknown);

    g_test_add_func ("/common-utils/validation/missing-type",        test_validation_missing_type);
    g_test_add_func ("/common-utils/validation/missing-label",       test_validation_missing_label);
    g_test_add_func ("/common-utils/validation/empty-label",         test_validation_empty_label);
    g_test_add_func ("/common-utils/validation/missing-secret",      test_validation_missing_secret);
    g_test_add_func ("/common-utils/validation/invalid-secret-b32",  test_validation_invalid_secret_base32);
    g_test_add_func ("/common-utils/validation/unknown-algorithm",   test_validation_unknown_algorithm);
    g_test_add_func ("/common-utils/validation/digits-below",        test_validation_digits_below_range);
    g_test_add_func ("/common-utils/validation/digits-above",        test_validation_digits_above_range);
    g_test_add_func ("/common-utils/validation/negative-period",     test_validation_negative_period);
    g_test_add_func ("/common-utils/validation/period-above-range",  test_validation_period_above_range);
    g_test_add_func ("/common-utils/validation/negative-counter",    test_validation_negative_hotp_counter);
    g_test_add_func ("/common-utils/validation/non-string-group",    test_validation_non_string_group);
    g_test_add_func ("/common-utils/validation/unsupported-type",    test_validation_unsupported_type);

    return g_test_run ();
}
