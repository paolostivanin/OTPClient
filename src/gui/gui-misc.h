#pragma once

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

gchar   *gui_misc_get_db_path_from_cfg   (void);

void     gui_misc_save_db_path_to_cfg    (const gchar *db_path);

void     gui_misc_send_notification      (GApplication *app,
                                          const gchar  *title,
                                          const gchar  *body);

G_END_DECLS
