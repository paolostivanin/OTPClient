#pragma once

#include "data.h"

G_BEGIN_DECLS

void        setup_dbus_listener (AppData    *app_data);

gboolean    check_inactivity    (gpointer    user_data);

G_END_DECLS
