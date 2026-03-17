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

typedef void (*PasswordDialogCallback) (const gchar *password,
                                        gpointer     user_data);

PasswordDialog *password_dialog_new           (PasswordDialogMode    mode,
                                               PasswordDialogCallback callback,
                                               gpointer              user_data);

G_END_DECLS
