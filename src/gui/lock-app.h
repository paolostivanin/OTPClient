#pragma once

#include <adwaita.h>
#include "otpclient-types.h"

G_BEGIN_DECLS

void lock_app_init_dbus_watchers   (OTPClientApplication *app);
void lock_app_cleanup              (OTPClientApplication *app);

void lock_app_lock                 (OTPClientApplication *app);
void lock_app_unlock               (OTPClientApplication *app);

void lock_app_reset_inactivity     (OTPClientApplication *app);

guint lock_app_get_dbus_watcher_id (OTPClientApplication *app);

G_END_DECLS
