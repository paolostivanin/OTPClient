#pragma once

#include <glib.h>
#include "autostart.h"

G_BEGIN_DECLS

/* What the reconciliation should do about one answer, worked out apart from the
 * doing of it.
 *
 * Everything around this rule needs a session bus, a portal that prompts and a
 * running application, none of which a test can stand up, and the rule is
 * exactly where the mistakes have been: which direction a key may be corrected
 * in, when a leftover entry is worth chasing, and how long the durable note
 * saying the desktop has not been told yet has to live. Lifting the table out
 * is what makes it something a test can walk. */
typedef struct {
    /* Write the autostart key, to autostart_value, and tell the user it moved.
     * Only ever set when that differs from what the key already says. */
    gboolean write_autostart;
    gboolean autostart_value;
    /* Turn minimize-to-tray off, and tell the user why. */
    gboolean disable_tray;
    /* Ask the desktop to take away an entry that may have been left behind by
     * an earlier run. Deliberately separate from write_autostart: it is a
     * second request, not a key. */
    gboolean retry_removal;
    /* Whether the durable marker is this answer's business at all. FALSE means
     * leave it exactly as it is, which is not the same as clearing it, and is
     * the whole reason this is a flag and not just pending_value. */
    gboolean write_pending;
    gboolean pending_value;
} AutostartActions;

/* wanted_autostart is what the request asked for, result is what came back.
 *
 * desktop_can_answer is autostart_is_supported(): a session with no Background
 * backend cannot be told anything, so there is no point noting that it still
 * has to be. Without this the marker below would re-issue a doomed request on
 * every launch, forever, on precisely the desktops that cannot serve it.
 *
 * key_autostart and key_minimize_to_tray are what the two keys currently say,
 * so that a correction is only reported when it is really a change. */
AutostartActions autostart_policy_decide (gboolean               wanted_autostart,
                                          const AutostartResult *result,
                                          gboolean               desktop_can_answer,
                                          gboolean               key_autostart,
                                          gboolean               key_minimize_to_tray);

/* Whether a D-Bus error is the desktop saying it does not have a Background
 * backend at all, as against not having managed something this time.
 *
 * It belongs beside the table above because it feeds the same decision: this is
 * what turns into desktop_can_answer, and answering it too readily retires a
 * durable note that only a real answer should retire. Which is why it is here
 * and not left inline at its call sites, where nothing could get at it. */
gboolean autostart_policy_error_means_no_backend (const GError *err);

G_END_DECLS
