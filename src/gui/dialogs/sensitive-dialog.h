#pragma once
#include <adwaita.h>

typedef void (*SensitiveDialogClearFunc) (AdwDialog *dialog);

void sensitive_dialog_setup (AdwDialog *dialog, SensitiveDialogClearFunc clear);
void sensitive_dialog_clear (AdwDialog *dialog);
gboolean sensitive_dialog_is_closed (AdwDialog *dialog);
GCancellable *sensitive_dialog_get_cancellable (AdwDialog *dialog);
