#pragma once

#include <gtk/gtk.h>
#include "data.h"

G_BEGIN_DECLS

gchar *prompt_for_password (AppData *app_data, gchar *current_key, const gchar *action_name, gboolean is_export_pwd);

G_END_DECLS
