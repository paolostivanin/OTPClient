#pragma once

#include <gtk/gtk.h>
#include "data.h"

G_BEGIN_DECLS

#define ACTION_OPEN 5
#define ACTION_SAVE 10

void select_file_icon_pressed_cb (GtkEntry         *entry,
                                  gint              position,
                                  GdkEventButton   *event,
                                  gpointer          data);

void update_cfg_file             (AppData *app_data);

G_END_DECLS