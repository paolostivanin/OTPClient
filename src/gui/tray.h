#pragma once

#ifdef ENABLE_MINIMIZE_TO_TRAY

#include "otpclient-application.h"

G_BEGIN_DECLS

void otpclient_tray_init    (OTPClientApplication *app);
void otpclient_tray_enable  (OTPClientApplication *app);
void otpclient_tray_disable (OTPClientApplication *app);
void otpclient_tray_cleanup (OTPClientApplication *app);

G_END_DECLS

#endif
