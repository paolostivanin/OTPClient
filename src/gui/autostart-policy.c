#include <gio/gio.h>
#include "autostart-policy.h"

/* The bar is deliberately high: a name nobody owns, an object that does not
 * carry the interface, a method that is not there. Those are the shapes x-d-p
 * makes when no backend implements Background, and they are the only ones that
 * can never come out differently on a later try.
 *
 * Everything else, a session bus that could not be reached, a call that timed
 * out, a portal restarting mid-request, is a bad moment rather than a verdict.
 * Reading one of those as "unsupported" clears the durable note, and the note is
 * the only thing that can bring anyone back to remove an entry the sandbox
 * cannot see. Guessing the other way costs one failed request on the next
 * launch, so that is the way to guess. */
gboolean
autostart_policy_error_means_no_backend (const GError *err)
{
    if (err == NULL)
        return FALSE;

    return g_error_matches (err, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN)
        || g_error_matches (err, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER)
        || g_error_matches (err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_INTERFACE)
        || g_error_matches (err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD)
        || g_error_matches (err, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_OBJECT);
}

AutostartActions
autostart_policy_decide (gboolean               wanted_autostart,
                         const AutostartResult *result,
                         gboolean               desktop_can_answer,
                         gboolean               key_autostart,
                         gboolean               key_minimize_to_tray)
{
    AutostartActions actions = { 0 };

    g_return_val_if_fail (result != NULL, actions);

    /* An answer about an intent that has since been restated is about nothing.
     * The request that restated it is still outstanding and owns every decision
     * below, the marker included: clearing it here would throw away a retry the
     * newer request has not earned yet, and setting it would leave one behind
     * after the newer request succeeds. */
    if (result->superseded)
        return actions;

    if (!result->autostart)
    {
        /* The entry is in the state that was not asked for, and the key has to
         * say which one that is.
         *
         * Correcting downwards, dropping a claim to an entry that was never
         * created, is always safe. Correcting upwards, admitting an entry is
         * still there after a removal failed, is the one that matters most,
         * since a key reading disabled over a live entry means the app starts
         * at login with no switch left to stop it. But upwards can only be done
         * on an answer: a refusal or a timeout means the entry is wherever it
         * already was, and switching the key on because a launch asked to
         * remove one and heard nothing back would hand the user a login entry
         * they never requested. */
        const gboolean desktop_is_at = !wanted_autostart;
        if ((result->answered || wanted_autostart) && key_autostart != desktop_is_at)
        {
            actions.write_autostart = TRUE;
            actions.autostart_value = desktop_is_at;
        }

        /* Only in the direction that can leave something behind, and only with
         * a desktop answering: we asked for an entry and did not get one, but
         * an entry from an earlier run may still be there, launching an app
         * that then gets killed. This cannot loop, since the removal reports
         * its own outcome and by then the key agrees with it. Asking after
         * silence would only buy more silence, and the marker below is the
         * right way to chase that one. */
        if (wanted_autostart && result->answered)
            actions.retry_removal = TRUE;
    }

    /* Closing to a tray the desktop will not let us live in hides the window
     * and then kills the process, which loses the tray as well. Better to keep
     * the window than to hand the user a disappearing act. */
    if (!result->background && key_minimize_to_tray)
        actions.disable_tray = TRUE;

    /* The marker is the only thing here that outlives the process, so it
     * carries every case this answer could not close.
     *
     * Set on anything inconclusive, whatever asked for it. Under Flatpak the
     * login entry lives outside the sandbox and cannot be read back, so an
     * unanswered removal is the state with no other way out: the key says off,
     * an entry may still be launching the app, and nothing in a later launch
     * would otherwise think to ask again.
     *
     * Cleared on an answer, including a refusal, because a desktop that said
     * what state it is in has been told and has replied; the keys were squared
     * with it above and there is nothing outstanding. Cleared just as firmly
     * when no backend exists, since a note that nobody can ever act on is only
     * a doomed request on every future launch.
     *
     * Except when this answer's own remedy is still to come. A leftover chase
     * is a second request that has not been sent yet, let alone answered, and
     * a quit between the two would leave the entry it was meant to take away
     * with nothing pointing at it. The answer that closes the chase is the one
     * entitled to retire this. */
    actions.write_pending = TRUE;
    actions.pending_value = desktop_can_answer
                            && (!result->answered || actions.retry_removal);

    return actions;
}
