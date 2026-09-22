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

/* As above, but the dialog takes ownership of user_data and releases it with
 * user_data_destroy when it is disposed, whatever the callback returns and
 * whether or not the callback ever runs. Use this for a heap context: a
 * dismissed dialog never calls back, so freeing from the callback alone leaks
 * the context every time the user presses Escape. The callback must not free
 * it, and must not assume it outlives the dialog. */
PasswordDialog *password_dialog_new_full      (PasswordDialogMode    mode,
                                               PasswordDialogCallback callback,
                                               gpointer              user_data,
                                               GDestroyNotify        user_data_destroy);

/* Attach an error message that is visible as soon as the dialog is presented,
 * before the user types anything. The async unlock path closes the dialog
 * while the worker runs and reopens it on a wrong-password retry, so there is
 * no synchronous callback return to carry the reason. Editing any field hides
 * the label again, same as for synchronous rejections. NULL or empty is a
 * no-op. */
void            password_dialog_set_initial_error (PasswordDialog *self,
                                                   const gchar    *message);

/* Configure this dialog for "locked database" presentation: adds a Quit
 * button at the start of the header bar that emits "quit-requested" when
 * clicked. The dialog stays dismissable, so Escape/X/click-outside fire the
 * normal "closed" signal; lock-state callers use that to drop to the locked
 * page rather than quit (#467). Idempotent. */
void            password_dialog_set_locked_mode (PasswordDialog *self);

G_END_DECLS
