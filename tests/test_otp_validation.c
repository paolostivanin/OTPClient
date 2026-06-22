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

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    gchar *init_err = init_libs (1024 * 1024);
    g_assert_null (init_err);

    g_test_add_func ("/validation/valid-root", test_valid_database_root);
    g_test_add_func ("/validation/root-array", test_root_must_be_array);
    g_test_add_func ("/validation/zero-period", test_zero_period_rejected);
    g_test_add_func ("/validation/hotp-overflow", test_hotp_counter_overflow_rejected);

    return g_test_run ();
}
