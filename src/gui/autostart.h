#pragma once

#include <glib.h>
#include "otpclient-types.h"

G_BEGIN_DECLS

/* Probe whether this session can start apps at login at all. Side-effect free
 * and asynchronous, so call it once from startup(); autostart_is_supported()
 * answers pessimistically until the probe lands. */
void     autostart_init          (void);

/* FALSE once we know nothing here can write an autostart entry. Under Flatpak
 * that means no backend implements org.freedesktop.impl.portal.Background,
 * which is the case on sway, Hyprland, river, LXQt, COSMIC and plain XFCE,
 * precisely the desktops most likely to want a tray icon. */
gboolean autostart_is_supported  (void);

/* What is now in effect, not merely that the call returned: the portal can
 * answer success and still have written nothing.
 *
 * One RequestBackground carries both answers and they are free to disagree, so
 * they are reported separately. A refused prompt denies both, but an older
 * x-d-p can fail the autostart write on its own with the background grant
 * intact.
 *
 * Both keys are reconciled here before any callback runs, precisely because one
 * request answers for both and no single caller is in a position to act on the
 * half it did not ask about. A callback is for the extra a caller wants on top,
 * a toast or a switch, and reads whichever half is its own business. */
typedef struct {
    /* The login-time entry is in the state that was asked for, whether that was
     * present or absent. On the host, that the file was written or removed.
     * FALSE also covers "never got an answer": nothing that could not be proven
     * is claimed, and the key is put back to the state the desktop is most
     * likely actually in. */
    gboolean autostart;
    /* The app may keep running with no window. Always TRUE on the host, which
     * has nothing that kills windowless apps. */
    gboolean background;
    /* The desktop said what state it is in, so a FALSE above means it is in the
     * other one and the key can be corrected in either direction. FALSE here is
     * a refusal, a timeout or a dead bus: nothing was learned, the entry is
     * wherever it already was, and the only correction that can be justified is
     * to stop a key claiming an entry that was never created. Getting this
     * wrong the other way would switch a login entry on every time a launch
     * asked the desktop to remove one and got no reply. */
    gboolean answered;
    /* This answer describes an intent that has already been restated, so it is
     * about nothing. Requests are serialised, so a superseded answer never
     * reached the portal or was already overtaken before the reconciliation
     * step; either way the fields above are meaningless and the only thing left
     * to do is release whatever the callback owns. */
    gboolean superseded;
} AutostartResult;

typedef void (*AutostartResultFunc) (const AutostartResult *result,
                                     gpointer               user_data);

/* Ask for the login-time launch to exist (or not), and under Flatpak for
 * permission to keep running in the background.
 *
 * Every caller goes through here and always states the full desired autostart
 * value, including the callers that only want the background grant. The portal
 * defaults the autostart option to FALSE, so a RequestBackground that omits it
 * deletes an existing entry.
 *
 * The entry's argv is built from the current start-minimized preference, so a
 * change to that preference has to come back through here.
 *
 * Serialised: only one request is ever outstanding, because two overlapping
 * RequestBackground calls with opposite intents are applied by the desktop in
 * whatever order it finishes them, and the loser wins. A call made while one is
 * in flight is held back, and holding back a second time discards the first,
 * since only the newest intent is worth sending. A discarded call still gets
 * its callback, marked superseded.
 *
 * Both startup keys are reconciled against the answer before done runs, so a
 * NULL callback is a complete and correct use of this function.
 *
 * done may be NULL, and may run before this function returns: the non-portal
 * build writes the file synchronously. */
void     autostart_apply         (OTPClientApplication *app,
                                  gboolean              enable_autostart,
                                  AutostartResultFunc   done,
                                  gpointer              user_data);

/* Under Flatpak, ask for permission to keep running with no window, restating
 * the current autostart preference so the entry survives the call. Without the
 * grant, x-d-p notifies the user unprompted and then SIGKILLs the app, which is
 * what a minimize-to-tray user hits today.
 *
 * Asks for nothing on the host, which has no background killer and which must
 * not get an autostart file it did not ask for; done still runs, granting both
 * halves, because there is genuinely nothing standing in the way there. */
void     autostart_ensure_background (OTPClientApplication *app,
                                      AutostartResultFunc   done,
                                      gpointer              user_data);

/* TRUE when a login-time entry may still be sitting there that the keys no
 * longer ask for, which is how a CLI settings import leaves things: it can
 * write autostart=false but cannot take the entry away.
 *
 * Only ever a maybe. On the host the entry is a file and this is exact; under
 * Flatpak the entry belongs to the host's config directory, which the sandbox
 * cannot see, so this answers FALSE. Answering TRUE there would mean a
 * RequestBackground on every launch, creating a permission entry for a feature
 * nobody asked for. What covers the sandbox is the startup-reconcile-pending
 * key: whoever writes the startup keys without going through the desktop says
 * so, instead of the next launch having to guess. */
gboolean autostart_entry_may_exist (void);

/* State both startup keys at the desktop. There is no read-back API, so the
 * GSettings keys are the only record of the entry and they go stale: revoking
 * "Run in Background" writes no to the permission store without deleting the
 * .desktop file, so the app autostarts and is then killed.
 *
 * Whichever key the desktop refuses is put back by the reconciliation every
 * request goes through, so there is nothing to pass and nothing to wait for.
 *
 * Called once per launch, and again after a settings import, which is the other
 * way the keys get set without anyone having asked the desktop. */
void     autostart_reassert      (OTPClientApplication *app);

G_END_DECLS
