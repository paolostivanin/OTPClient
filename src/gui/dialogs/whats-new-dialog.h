#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define WHATS_NEW_TYPE_DIALOG (whats_new_dialog_get_type ())

G_DECLARE_FINAL_TYPE (WhatsNewDialog, whats_new_dialog, WHATS_NEW, DIALOG, AdwDialog)

WhatsNewDialog *whats_new_dialog_new (gboolean is_welcome);

G_END_DECLS
