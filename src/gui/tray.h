#pragma once

#include <gtk/gtk.h>
#include "data.h"

G_BEGIN_DECLS

void init_tray_icon            (AppData *app_data);
void switch_tray_use           (AppData *app_data);

G_END_DECLS
