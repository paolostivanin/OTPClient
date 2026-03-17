#pragma once

#include <adwaita.h>
#include "otpclient-application.h"

G_BEGIN_DECLS

#define SETTINGS_TYPE_DIALOG (settings_dialog_get_type ())

G_DECLARE_FINAL_TYPE (SettingsDialog, settings_dialog, SETTINGS, DIALOG, AdwPreferencesDialog)

SettingsDialog *settings_dialog_new (OTPClientApplication *app);

G_END_DECLS
