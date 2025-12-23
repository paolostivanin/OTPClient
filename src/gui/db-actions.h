#pragma once

#include "gtk-compat.h"
#include "data.h"

G_BEGIN_DECLS

#define ACTION_OPEN 5
#define ACTION_SAVE 10

void select_file_icon_pressed_cb (GtkEntry       *entry,
                                  gint            position,
                                  GdkEvent       *event,
                                  gpointer        data);

void update_cfg_file             (AppData        *app_data);

void revert_db_path              (AppData        *app_data,
                                  gchar          *old_db_path);

G_END_DECLS
