#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define PASSWORD_TYPE_DIALOG (password_dialog_get_type ())

G_DECLARE_FINAL_TYPE (PasswordDialog, password_dialog, PASSWORD, DIALOG, AdwDialog)

typedef enum
{
    PASSWORD_MODE_DECRYPT,
    PASSWORD_MODE_NEW,
    PASSWORD_MODE_CHANGE
} PasswordDialogMode;

typedef gboolean (*PasswordDialogCallback) (const gchar *current_password,
                                            const gchar *password,
                                            gchar      **error_message,
                                            gpointer     user_data);

PasswordDialog *password_dialog_new           (PasswordDialogMode    mode,
                                               PasswordDialogCallback callback,
                                               gpointer              user_data);

/* Configure this dialog for "locked database" presentation: adds a Quit
 * button at the start of the header bar that emits "quit-requested" when
 * clicked. The dialog stays dismissable, so Escape/X/click-outside fire the
 * normal "closed" signal; lock-state callers use that to drop to the locked
 * page rather than quit (#467). Idempotent. */
void            password_dialog_set_locked_mode (PasswordDialog *self);

G_END_DECLS
