#include <glib.h>
#include <cotp.h>
#include "common.h"
#include "otp-entry.h"

static void
test_steam_generation (void)
{
    OTPEntry *entry = otp_entry_new (
        "alice", "Steam", NULL, "TOTP", 30, 0, "SHA1", 6,
        "JBSWY3DPEHPK3PXP");
    otp_entry_update_otp (entry);

    cotp_error_t err = NO_ERROR;
    gchar *expected = get_steam_totp ("JBSWY3DPEHPK3PXP", 30, &err);
    g_assert_cmpint (err, ==, NO_ERROR);
    g_assert_cmpstr (otp_entry_get_otp_value (entry), ==, expected);

    sensitive_free (expected);
    g_object_unref (entry);
}

/* The window's refresh tick decides rotation by comparing against the step
 * recorded when the code was generated, not against a value updated only on
 * ticks. Generating must therefore record the current step. */
static void
test_rendered_step_tracks_generation (void)
{
    OTPEntry *entry = otp_entry_new ("alice", "Example", NULL, "TOTP", 30, 0,
                                     "SHA1", 6, "JBSWY3DPEHPK3PXP");
    g_assert_cmpint (otp_entry_get_last_rendered_step (entry), ==, 0);

    gint64 before = g_get_real_time () / G_USEC_PER_SEC;
    otp_entry_update_otp (entry);
    gint64 after = g_get_real_time () / G_USEC_PER_SEC;

    gint64 step = otp_entry_get_last_rendered_step (entry);
    g_assert_cmpint (step, >=, before / 30);
    g_assert_cmpint (step, <=, after / 30);

    otp_entry_set_last_rendered_step (entry, 4242);
    g_assert_cmpint (otp_entry_get_last_rendered_step (entry), ==, 4242);

    g_object_unref (entry);
}

/* Generate immediately on either side of a period boundary and assert that
 * both the code and last_rendered_step come from the exact same timestamp. */
static void
test_rendered_step_exact_boundary (void)
{
    const gchar *secret = "JBSWY3DPEHPK3PXP";
    OTPEntry *entry = otp_entry_new ("alice", "Example", NULL, "TOTP", 30, 0,
                                     "SHA1", 6, secret);
    cotp_error_t err;

    const gint64 before = 3000 - 1;
    otp_entry_test_update_otp_at (entry, before);
    gchar *expected = get_totp_at (secret, (long) before, 6, 30,
                                   COTP_SHA1, &err);
    g_assert_nonnull (expected);
    g_assert_cmpstr (otp_entry_get_otp_value (entry), ==, expected);
    g_assert_cmpint (otp_entry_get_last_rendered_step (entry), ==, 99);
    sensitive_free (expected);

    const gint64 after = 3000;
    otp_entry_test_update_otp_at (entry, after);
    expected = get_totp_at (secret, (long) after, 6, 30,
                            COTP_SHA1, &err);
    g_assert_nonnull (expected);
    g_assert_cmpstr (otp_entry_get_otp_value (entry), ==, expected);
    g_assert_cmpint (otp_entry_get_last_rendered_step (entry), ==, 100);
    sensitive_free (expected);

    g_object_unref (entry);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    gchar *init_err = init_libs (DEFAULT_MEMLOCK_VALUE);
    g_assert_null (init_err);
    g_test_add_func ("/otp-entry/steam", test_steam_generation);
    g_test_add_func ("/otp-entry/rendered-step", test_rendered_step_tracks_generation);
    g_test_add_func ("/otp-entry/rendered-step-boundary", test_rendered_step_exact_boundary);
    return g_test_run ();
}
