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

/* Configure this dialog for "locked database" presentation:
 *  - shows a Quit button at the start of the header bar
 *  - sets can_close to FALSE so stray Escape/X don't dismiss the prompt
 *  - emits "quit-requested" when the user clicks Quit, presses Escape,
 *    clicks the dialog's X, or clicks the parent window's close button
 * Idempotent. */
void            password_dialog_set_locked_mode (PasswordDialog *self);

G_END_DECLS
