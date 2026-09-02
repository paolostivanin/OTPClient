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

/* granted reports the state that is now in effect, not merely that the call
 * returned: the portal can answer success and still have written nothing. */
typedef void (*AutostartResultFunc) (gboolean granted,
                                     gpointer user_data);

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
 * A no-op on the host, which has no background killer, and which must not get
 * an autostart file it did not ask for. */
void     autostart_ensure_background (OTPClientApplication *app);

/* Call once per launch. There is no read-back API, so the GSettings key is the
 * only record of the entry and it goes stale: revoking "Run in Background"
 * writes no to the permission store without deleting the .desktop file, so the
 * app autostarts and is then killed. Re-assert; if the answer is no, clear the
 * key and remove the entry. */
void     autostart_reassert      (OTPClientApplication *app);

G_END_DECLS
