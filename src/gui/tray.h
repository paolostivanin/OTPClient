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

/* FALSE only once we know there is no StatusNotifierWatcher on the session bus,
 * so the UI can tell the user the feature won't work here. Callers that need a
 * hard guarantee an icon exists must not rely on this. */
gboolean otpclient_tray_is_available (void);

G_END_DECLS

#endif
