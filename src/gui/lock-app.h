#pragma once

#include "data.h"

G_BEGIN_DECLS

void        lock_app            (GtkWidget  *w,
                                 gpointer    user_data);

void        setup_dbus_listener (AppData    *app_data);

gboolean    check_inactivity    (gpointer    user_data);

G_END_DECLS
