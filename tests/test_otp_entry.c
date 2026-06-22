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

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    gchar *init_err = init_libs (DEFAULT_MEMLOCK_VALUE);
    g_assert_null (init_err);
    g_test_add_func ("/otp-entry/steam", test_steam_generation);
    return g_test_run ();
}
