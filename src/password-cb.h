#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

gchar *prompt_for_password (GtkWidget *main_window, gboolean file_exists);

G_END_DECLS