/* The reconciliation's decision table.
 *
 * One RequestBackground answers for both startup keys, and the answer can be a
 * grant, a refusal, a timeout, a dead bus, or about an intent that has already
 * been restated. Nothing can be read back afterwards: under Flatpak the login
 * entry belongs to a config directory outside the sandbox, so the keys and a
 * durable "not told yet" marker are the entire memory of what the desktop was
 * asked and what it said. Get a cell of this table wrong and the app either
 * starts at login with no switch left to stop it, or turns a login entry on for
 * someone who never asked for one.
 *
 * The machinery around the rule needs a portal, a session bus and a running
 * application, which is why the rule was lifted out into autostart_policy_decide
 * where it can be walked case by case. What is still not covered here is the
 * ordering machinery itself, the queue and the Request.Close calls: that needs a
 * fake xdg-desktop-portal and is not attempted. */

#include <glib.h>
#include <gio/gio.h>
#include "autostart-policy.h"

/* The desktop said what it did. Everything else is a variation on this. */
static AutostartResult
answered (gboolean autostart, gboolean background)
{
    AutostartResult r = { 0 };
    r.autostart = autostart;
    r.background = background;
    r.answered = TRUE;
    return r;
}

/* A refusal, a timeout or a dead bus: nothing was learned. */
static AutostartResult
unanswered (void)
{
    AutostartResult r = { 0 };
    return r;
}

/* Asking for an entry and getting one settles everything and leaves no note. */
static void
test_granted_enable_is_quiet (void)
{
    const AutostartResult r = answered (TRUE, TRUE);
    const AutostartActions a = autostart_policy_decide (TRUE, &r, TRUE, TRUE, TRUE);

    g_assert_false (a.write_autostart);
    g_assert_false (a.disable_tray);
    g_assert_false (a.retry_removal);
    g_assert_true (a.write_pending);
    g_assert_false (a.pending_value);
}

/* And so does asking for it to go away and having it go away. */
static void
test_granted_disable_is_quiet (void)
{
    const AutostartResult r = answered (TRUE, TRUE);
    const AutostartActions a = autostart_policy_decide (FALSE, &r, TRUE, FALSE, TRUE);

    g_assert_false (a.write_autostart);
    g_assert_false (a.retry_removal);
    g_assert_true (a.write_pending);
    g_assert_false (a.pending_value);
}

/* The desktop was asked for an entry and said no. The key must stop claiming
 * one, and the removal chases an entry an earlier run may have left behind. */
static void
test_answered_refusal_to_create_corrects_downwards (void)
{
    const AutostartResult r = answered (FALSE, TRUE);
    const AutostartActions a = autostart_policy_decide (TRUE, &r, TRUE, TRUE, FALSE);

    g_assert_true (a.write_autostart);
    g_assert_false (a.autostart_value);
    g_assert_true (a.retry_removal);
}

/* An answer normally retires the note, but not this one: the chase it asked for
 * is a second request that has not been sent yet. Quit in between, and the entry
 * it was going to take away is left with nothing pointing at it. */
static void
test_a_pending_chase_keeps_the_marker (void)
{
    const AutostartResult r = answered (FALSE, TRUE);
    const AutostartActions a = autostart_policy_decide (TRUE, &r, TRUE, TRUE, FALSE);

    g_assert_true (a.retry_removal);
    g_assert_true (a.write_pending);
    g_assert_true (a.pending_value);
}

/* And the chase, once answered, is what retires it. This is that second call:
 * a removal, answered, carried out. Nothing is outstanding any more. */
static void
test_the_chase_being_answered_retires_the_marker (void)
{
    const AutostartResult r = answered (TRUE, TRUE);
    const AutostartActions a = autostart_policy_decide (FALSE, &r, TRUE, FALSE, FALSE);

    g_assert_false (a.retry_removal);
    g_assert_true (a.write_pending);
    g_assert_false (a.pending_value);
}

/* The one that matters most: a removal the desktop answered and did not carry
 * out. The entry is still there, so the key has to admit it, or the app starts
 * at login with the switch reading off and no way to stop it. */
static void
test_answered_failure_to_remove_corrects_upwards (void)
{
    const AutostartResult r = answered (FALSE, TRUE);
    const AutostartActions a = autostart_policy_decide (FALSE, &r, TRUE, FALSE, FALSE);

    g_assert_true (a.write_autostart);
    g_assert_true (a.autostart_value);
    /* Only creations chase a leftover. This one already is the removal. */
    g_assert_false (a.retry_removal);
}

/* Silence is not an answer. A launch-time re-assert speculatively asks for the
 * entry to be removed; if nothing comes back, switching the key on would hand
 * a login entry to someone who never asked for one. */
static void
test_unanswered_removal_leaves_the_key_alone (void)
{
    const AutostartResult r = unanswered ();
    const AutostartActions a = autostart_policy_decide (FALSE, &r, TRUE, FALSE, FALSE);

    g_assert_false (a.write_autostart);
    g_assert_false (a.retry_removal);
}

/* ...but it does have to be remembered. This is the case with no other way out:
 * the key says off, an entry outside the sandbox may still be launching the app,
 * and only a note that outlives the process gets it asked about again. */
static void
test_unanswered_removal_keeps_the_retry (void)
{
    const AutostartResult r = unanswered ();
    const AutostartActions a = autostart_policy_decide (FALSE, &r, TRUE, FALSE, FALSE);

    g_assert_true (a.write_pending);
    g_assert_true (a.pending_value);
}

/* An unanswered creation is still worth taking the claim back for: we asked and
 * cannot show anything for it, so the key must not say we got it. No leftover
 * chase, though, because asking after silence only buys more silence. */
static void
test_unanswered_creation_drops_the_claim_but_does_not_chase (void)
{
    const AutostartResult r = unanswered ();
    const AutostartActions a = autostart_policy_decide (TRUE, &r, TRUE, TRUE, FALSE);

    g_assert_true (a.write_autostart);
    g_assert_false (a.autostart_value);
    g_assert_false (a.retry_removal);
    g_assert_true (a.pending_value);
}

/* A desktop with no Background backend can never be told anything. Keeping the
 * note would mean a doomed portal request on every launch from here on, on
 * exactly the desktops that cannot serve one. */
static void
test_no_backend_retires_the_marker (void)
{
    const AutostartResult r = unanswered ();
    const AutostartActions a = autostart_policy_decide (FALSE, &r, FALSE, FALSE, FALSE);

    g_assert_true (a.write_pending);
    g_assert_false (a.pending_value);
}

/* A superseded answer describes an intent that has already been restated. The
 * request that restated it is still outstanding and owns every decision,
 * including the marker: clearing it here would discard a retry the newer
 * request has not earned, and setting it would strand one after it succeeds. */
static void
test_superseded_decides_nothing (void)
{
    AutostartResult r = answered (FALSE, FALSE);
    r.superseded = TRUE;

    const AutostartActions a = autostart_policy_decide (TRUE, &r, TRUE, TRUE, TRUE);

    g_assert_false (a.write_autostart);
    g_assert_false (a.disable_tray);
    g_assert_false (a.retry_removal);
    g_assert_false (a.write_pending);
}

/* The halves are independent: one request carries both, and an older portal can
 * fail the autostart write with the background grant perfectly intact. */
static void
test_background_refusal_disables_the_tray (void)
{
    const AutostartResult r = answered (TRUE, FALSE);
    const AutostartActions a = autostart_policy_decide (TRUE, &r, TRUE, TRUE, TRUE);

    g_assert_true (a.disable_tray);
    g_assert_false (a.write_autostart);
}

static void
test_autostart_refusal_leaves_the_tray_alone (void)
{
    const AutostartResult r = answered (FALSE, TRUE);
    const AutostartActions a = autostart_policy_decide (TRUE, &r, TRUE, TRUE, TRUE);

    g_assert_false (a.disable_tray);
    g_assert_true (a.write_autostart);
}

/* Nothing to turn off means nothing to report. */
static void
test_background_refusal_is_quiet_when_the_tray_is_already_off (void)
{
    const AutostartResult r = answered (TRUE, FALSE);
    const AutostartActions a = autostart_policy_decide (TRUE, &r, TRUE, TRUE, FALSE);

    g_assert_false (a.disable_tray);
}

/* A correction is only a correction when something moves. The caller shows a
 * toast for every write, so a redundant one would announce a refusal on a key
 * that has read that way all along. */
static void
test_a_key_already_in_the_right_state_is_not_rewritten (void)
{
    const AutostartResult r = answered (FALSE, TRUE);
    const AutostartActions a = autostart_policy_decide (TRUE, &r, TRUE, FALSE, FALSE);

    g_assert_false (a.write_autostart);
    /* The leftover chase is about the entry, not the key, so it still stands. */
    g_assert_true (a.retry_removal);
}

/* The other half of the same decision: what feeds desktop_can_answer.
 *
 * x-d-p does not export the Background interface unless a backend implements
 * it, so an error naming the service, interface or method absent is the desktop
 * saying it can never do this. */
static void
test_absence_is_recognised (void)
{
    const gint definitive[] = {
        G_DBUS_ERROR_SERVICE_UNKNOWN,
        G_DBUS_ERROR_NAME_HAS_NO_OWNER,
        G_DBUS_ERROR_UNKNOWN_INTERFACE,
        G_DBUS_ERROR_UNKNOWN_METHOD,
        G_DBUS_ERROR_UNKNOWN_OBJECT,
    };

    for (gsize i = 0; i < G_N_ELEMENTS (definitive); i++)
    {
        g_autoptr (GError) err = g_error_new_literal (G_DBUS_ERROR, definitive[i], "absent");
        g_assert_true (autostart_policy_error_means_no_backend (err));
    }
}

/* And the failures that are a bad moment rather than a verdict. Every one of
 * these used to retire the durable marker, which is how a bus that was briefly
 * unreachable took away the retry that removes an entry the sandbox cannot see.
 * The cost of getting this wrong the other way is one failed request. */
static void
test_a_bad_moment_is_not_absence (void)
{
    const gint transient[] = {
        G_DBUS_ERROR_NO_REPLY,
        G_DBUS_ERROR_TIMEOUT,
        G_DBUS_ERROR_TIMED_OUT,
        G_DBUS_ERROR_DISCONNECTED,
        G_DBUS_ERROR_LIMITS_EXCEEDED,
        /* A bus policy standing in the way is not a missing backend either. */
        G_DBUS_ERROR_ACCESS_DENIED,
        G_DBUS_ERROR_FAILED,
    };

    for (gsize i = 0; i < G_N_ELEMENTS (transient); i++)
    {
        g_autoptr (GError) err = g_error_new_literal (G_DBUS_ERROR, transient[i], "not this time");
        g_assert_false (autostart_policy_error_means_no_backend (err));
    }

    /* g_bus_get_sync reports a session bus it cannot reach in this domain, and
     * that is a statement about the connection, not about the portal. */
    g_autoptr (GError) io = g_error_new_literal (G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "no bus");
    g_assert_false (autostart_policy_error_means_no_backend (io));

    g_assert_false (autostart_policy_error_means_no_backend (NULL));
}

int
main (int argc, char *argv[])
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/autostart-policy/granted/enable", test_granted_enable_is_quiet);
    g_test_add_func ("/autostart-policy/granted/disable", test_granted_disable_is_quiet);

    g_test_add_func ("/autostart-policy/answered/refused-creation",
                     test_answered_refusal_to_create_corrects_downwards);
    g_test_add_func ("/autostart-policy/answered/failed-removal",
                     test_answered_failure_to_remove_corrects_upwards);
    g_test_add_func ("/autostart-policy/answered/chase-keeps-the-marker",
                     test_a_pending_chase_keeps_the_marker);
    g_test_add_func ("/autostart-policy/answered/answered-chase-retires-it",
                     test_the_chase_being_answered_retires_the_marker);

    g_test_add_func ("/autostart-policy/silence/removal-keeps-the-key",
                     test_unanswered_removal_leaves_the_key_alone);
    g_test_add_func ("/autostart-policy/silence/removal-keeps-the-retry",
                     test_unanswered_removal_keeps_the_retry);
    g_test_add_func ("/autostart-policy/silence/creation-drops-the-claim",
                     test_unanswered_creation_drops_the_claim_but_does_not_chase);
    g_test_add_func ("/autostart-policy/silence/no-backend-retires-the-marker",
                     test_no_backend_retires_the_marker);

    g_test_add_func ("/autostart-policy/superseded/decides-nothing",
                     test_superseded_decides_nothing);

    g_test_add_func ("/autostart-policy/background/refusal-disables-tray",
                     test_background_refusal_disables_the_tray);
    g_test_add_func ("/autostart-policy/background/autostart-refusal-spares-tray",
                     test_autostart_refusal_leaves_the_tray_alone);
    g_test_add_func ("/autostart-policy/background/already-off-is-quiet",
                     test_background_refusal_is_quiet_when_the_tray_is_already_off);

    g_test_add_func ("/autostart-policy/no-op/key-already-right",
                     test_a_key_already_in_the_right_state_is_not_rewritten);

    g_test_add_func ("/autostart-policy/backend/absence-is-recognised",
                     test_absence_is_recognised);
    g_test_add_func ("/autostart-policy/backend/a-bad-moment-is-not-absence",
                     test_a_bad_moment_is_not_absence);

    return g_test_run ();
}
