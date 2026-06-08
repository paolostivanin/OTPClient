#pragma once

#include <adwaita.h>
#include "otpclient-types.h"
#include "dialogs/password-dialog.h"

G_BEGIN_DECLS

void lock_app_init_dbus_watchers   (OTPClientApplication *app);
void lock_app_cleanup              (OTPClientApplication *app);

void lock_app_lock                 (OTPClientApplication *app);
void lock_app_unlock               (OTPClientApplication *app);

void lock_app_present_unlock_dialog (OTPClientApplication *app);

/* Configure dlg as the unlock dialog for a locked database:
 * adds the Quit button, blocks accidental dismissal, and routes any
 * quit attempt (Quit button, Escape, dialog X, parent window X,
 * taskbar Close) to g_application_quit(app). */
void lock_app_install_unlock_dialog_quit (PasswordDialog       *dlg,
                                          OTPClientApplication *app);

void lock_app_reset_inactivity     (OTPClientApplication *app);

G_END_DECLS
