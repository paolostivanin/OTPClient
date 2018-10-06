#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

gchar *prompt_for_password (const gchar *db_path, gchar *current_key);

G_END_DECLS