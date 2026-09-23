#pragma once

#include <adwaita.h>
#include "otpclient-types.h"
#include "dialogs/password-dialog.h"

G_BEGIN_DECLS

void lock_app_init_dbus_watchers   (OTPClientApplication *app);
void lock_app_cleanup              (OTPClientApplication *app);

/* Desktop screensaver state, independent of the database's Auto-Lock setting. */
gboolean lock_app_get_session_locked (OTPClientApplication *app);

void lock_app_lock                 (OTPClientApplication *app);
void lock_app_unlock               (OTPClientApplication *app);

/* Enter the locked state (purge the key, show the locked page, disable DB
 * actions) WITHOUT presenting the unlock dialog. No-op when already locked.
 * lock_app_lock is this plus a fresh unlock dialog; the unlock dialog's
 * dismiss handler uses it to drop to the locked page (#467). */
void lock_app_enter_locked_state   (OTPClientApplication *app);

void lock_app_present_unlock_dialog (OTPClientApplication *app);

/* Same, but seeds the prompt with an error message (e.g. a wrong-password
 * retry from the async unlock worker). NULL shows a clean dialog. */
void lock_app_present_unlock_dialog_with_error (OTPClientApplication *app,
                                                const gchar          *error_message);

/* Wire dlg up as the unlock dialog for a locked database: adds the Quit
 * button (routes to g_application_quit) and a "closed" handler that drops the
 * app to the locked page when the user dismisses the prompt without unlocking
 * (Escape, dialog X, parent window X). */
void lock_app_install_unlock_dialog_quit (PasswordDialog       *dlg,
                                          OTPClientApplication *app);

void lock_app_reset_inactivity     (OTPClientApplication *app);

G_END_DECLS
