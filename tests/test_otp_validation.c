#include <glib.h>
#include <jansson.h>
#include "common.h"
#include "gquarks.h"
#include "otp-validation.h"

static json_t *
valid_totp (void)
{
    return build_json_obj ("TOTP", "alice", "Example", "JBSWY3DPEHPK3PXP",
                           6, "SHA1", 30, 0, NULL);
}

static void
test_valid_database_root (void)
{
    json_t *root = json_array ();
    json_array_append_new (root, valid_totp ());

    GError *err = NULL;
    g_assert_true (otp_validate_database_root (root, &err));
    g_assert_no_error (err);
    json_decref (root);
}

static void
test_issuer_only_accepted (void)
{
    /* A token with an issuer but no label must load (issue #458). */
    json_t *root = json_array ();
    json_t *obj = valid_totp ();
    json_object_set_new (obj, "label", json_string (""));
    json_array_append_new (root, obj);

    GError *err = NULL;
    g_assert_true (otp_validate_database_root (root, &err));
    g_assert_no_error (err);
    json_decref (root);
}

static void
test_root_must_be_array (void)
{
    json_t *root = json_object ();
    GError *err = NULL;
    g_assert_false (otp_validate_database_root (root, &err));
    g_assert_error (err, generic_error_gquark (), GENERIC_ERRCODE);
    g_clear_error (&err);
    json_decref (root);
}

static void
test_zero_period_rejected (void)
{
    json_t *root = json_array ();
    json_t *obj = valid_totp ();
    json_object_set_new (obj, "period", json_integer (0));
    json_array_append_new (root, obj);

    GError *err = NULL;
    g_assert_false (otp_validate_database_root (root, &err));
    g_assert_error (err, generic_error_gquark (), GENERIC_ERRCODE);
    g_clear_error (&err);
    json_decref (root);
}

static void
test_hotp_counter_overflow_rejected (void)
{
    json_t *root = json_array ();
    json_t *obj = build_json_obj ("HOTP", "alice", "Example",
                                  "JBSWY3DPEHPK3PXP", 6, "SHA1",
                                  30, OTP_HOTP_COUNTER_MAX, NULL);
    json_array_append_new (root, obj);

    GError *err = NULL;
    g_assert_false (otp_validate_database_root (root, &err));
    g_assert_error (err, generic_error_gquark (), GENERIC_ERRCODE);
    g_clear_error (&err);
    json_decref (root);
}

static void
test_anonymous_repaired_then_valid (void)
{
    /* A token with neither a label nor an issuer is rejected outright, but the
     * load path repairs it first so the whole database still opens (issue #462). */
    json_t *root = json_array ();
    json_t *obj = valid_totp ();
    json_object_set_new (obj, "label", json_string (""));
    json_object_set_new (obj, "issuer", json_string (""));
    json_array_append_new (root, obj);

    GError *err = NULL;
    g_assert_false (otp_validate_database_root (root, &err));
    g_clear_error (&err);

    g_assert_cmpuint (otp_repair_database_root (root), ==, 1);
    g_assert_true (otp_validate_database_root (root, &err));
    g_assert_no_error (err);

    json_t *repaired = json_array_get (root, 0);
    g_assert_cmpstr (json_string_value (json_object_get (repaired, "label")), ==, "Unknown 0");

    /* Idempotent: a second pass finds nothing anonymous and changes nothing. */
    g_assert_cmpuint (otp_repair_database_root (root), ==, 0);
    g_assert_cmpstr (json_string_value (json_object_get (repaired, "label")), ==, "Unknown 0");

    json_decref (root);
}

static void
test_repair_leaves_named_tokens (void)
{
    /* Labeled and issuer-only tokens must be left untouched. */
    json_t *root = json_array ();
    json_array_append_new (root, valid_totp ());
    json_t *issuer_only = valid_totp ();
    json_object_set_new (issuer_only, "label", json_string (""));
    json_array_append_new (root, issuer_only);

    g_assert_cmpuint (otp_repair_database_root (root), ==, 0);
    g_assert_cmpstr (json_string_value (json_object_get (json_array_get (root, 0), "label")), ==, "alice");
    g_assert_cmpstr (json_string_value (json_object_get (json_array_get (root, 1), "label")), ==, "");

    json_decref (root);
}

static void
test_repair_indexes_by_position (void)
{
    /* The placeholder carries the token's array index, matching the "Token N"
     * position that validation used to report. */
    json_t *root = json_array ();
    json_array_append_new (root, valid_totp ());          /* index 0: named */
    json_t *anon = valid_totp ();
    json_object_set_new (anon, "label", json_string (""));
    json_object_set_new (anon, "issuer", json_string (""));
    json_array_append_new (root, anon);                   /* index 1: anonymous */

    g_assert_cmpuint (otp_repair_database_root (root), ==, 1);
    g_assert_cmpstr (json_string_value (json_object_get (json_array_get (root, 1), "label")), ==, "Unknown 1");

    json_decref (root);
}

static void
test_repair_import_token (void)
{
    otp_t otp = {0};
    otp.type = g_strdup ("TOTP");
    otp.algo = g_strdup ("SHA1");
    otp.digits = 6;
    otp.period = 30;
    otp.secret = g_strdup ("JBSWY3DPEHPK3PXP");

    g_assert_true (otp_repair_anonymous_import_token (&otp, 3));
    g_assert_cmpstr (otp.account_name, ==, "Unknown 3");

    GError *err = NULL;
    g_assert_true (otp_validate_import_token (&otp, &err));
    g_assert_no_error (err);

    /* Idempotent: an already-named token is not renamed. */
    g_assert_false (otp_repair_anonymous_import_token (&otp, 9));
    g_assert_cmpstr (otp.account_name, ==, "Unknown 3");

    g_free (otp.type);
    g_free (otp.algo);
    g_free (otp.account_name);
    g_free (otp.secret);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    gchar *init_err = init_libs (1024 * 1024);
    g_assert_null (init_err);

    g_test_add_func ("/validation/valid-root", test_valid_database_root);
    g_test_add_func ("/validation/issuer-only", test_issuer_only_accepted);
    g_test_add_func ("/validation/root-array", test_root_must_be_array);
    g_test_add_func ("/validation/zero-period", test_zero_period_rejected);
    g_test_add_func ("/validation/hotp-overflow", test_hotp_counter_overflow_rejected);
    g_test_add_func ("/validation/anonymous-repaired", test_anonymous_repaired_then_valid);
    g_test_add_func ("/validation/repair-leaves-named", test_repair_leaves_named_tokens);
    g_test_add_func ("/validation/repair-index", test_repair_indexes_by_position);
    g_test_add_func ("/validation/repair-import-token", test_repair_import_token);

    return g_test_run ();
}
