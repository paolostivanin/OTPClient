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

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    gchar *init_err = init_libs (DEFAULT_MEMLOCK_VALUE);
    g_assert_null (init_err);
    g_test_add_func ("/otp-entry/steam", test_steam_generation);
    g_test_add_func ("/otp-entry/rendered-step", test_rendered_step_tracks_generation);
    return g_test_run ();
}
