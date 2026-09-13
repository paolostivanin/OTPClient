#pragma once

#ifdef ENABLE_MINIMIZE_TO_TRAY

#include "otpclient-application.h"

G_BEGIN_DECLS

void otpclient_tray_init    (OTPClientApplication *app);
void otpclient_tray_enable  (OTPClientApplication *app);
void otpclient_tray_disable (OTPClientApplication *app);
void otpclient_tray_cleanup (OTPClientApplication *app);

/* Start with the window already tucked away, exactly as if the user had closed
 * it into the tray, so every existing no-icon path un-hides it again. Also arms
 * a deadline for the one failure the code cannot detect: a watcher that owns the
 * name but never answers and never signals. Call after otpclient_tray_init. */
void otpclient_tray_begin_hidden (OTPClientApplication *app);

/* The lock state changed: refresh the item's Title so the tooltip says why
 * clicking the icon is about to ask for a password. No-op when not published. */
void otpclient_tray_notify_locked_changed (OTPClientApplication *app);

/* The window is on screen again by some route other than the tray icon: a
 * second otpclient invocation, a search-provider activation, a D-Bus activate.
 * The tray's record of having tucked it away is what arms both the
 * stranded-window deadline and the un-hide on a lost panel, so it has to be
 * told, or both of those end up aimed at a window that is already visible. */
void otpclient_tray_notify_window_shown (OTPClientApplication *app);

/* FALSE only once we know there is no StatusNotifierWatcher on the session bus,
 * so the UI can tell the user the feature won't work here. Callers that need a
 * hard guarantee an icon exists must not rely on this. */
gboolean otpclient_tray_is_available (void);

/* Whether a StatusNotifierWatcher owns its name right now, asked of the bus
 * daemon and waited for. For the one caller that cannot wait for the name
 * watcher: resolve_start_hidden runs inside startup(), before the main loop has
 * turned over even once, so otpclient_tray_is_available() can only answer
 * "maybe" there. Everything else should use that instead. */
gboolean otpclient_tray_watcher_present_sync (void);

G_END_DECLS

#endif
