#pragma once

#include <gtk/gtk.h>
#include "data.h"

G_BEGIN_DECLS

#define OTPCLIENT_TYPE_WINDOW (otpclient_window_get_type ())

G_DECLARE_FINAL_TYPE (OtpclientWindow, otpclient_window,
                      OTPCLIENT, WINDOW, GtkApplicationWindow)

OtpclientWindow *otpclient_window_new (GtkApplication *app,
                                       gint            width,
                                       gint            height,
                                       AppData        *app_data);

/* Builder accessors — ownership stays with the window; callers must not unref */
GtkBuilder *otpclient_window_get_builder                  (OtpclientWindow *self);
GtkBuilder *otpclient_window_get_add_popover_builder      (OtpclientWindow *self);
GtkBuilder *otpclient_window_get_settings_popover_builder (OtpclientWindow *self);

G_END_DECLS
