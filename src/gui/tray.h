#pragma once

#ifdef ENABLE_MINIMIZE_TO_TRAY

#include "otpclient-application.h"

G_BEGIN_DECLS

void otpclient_tray_init    (OTPClientApplication *app);
void otpclient_tray_enable  (OTPClientApplication *app);
void otpclient_tray_disable (OTPClientApplication *app);
void otpclient_tray_cleanup (OTPClientApplication *app);

/* FALSE only once we know there is no StatusNotifierWatcher on the session bus,
 * so the UI can tell the user the feature won't work here. Callers that need a
 * hard guarantee an icon exists must not rely on this. */
gboolean otpclient_tray_is_available (void);

G_END_DECLS

#endif
