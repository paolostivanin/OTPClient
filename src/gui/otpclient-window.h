#pragma once

#include <gtk/gtk.h>
#include "data.h"

G_BEGIN_DECLS

#define OTPCLIENT_TYPE_WINDOW (otpclient_window_get_type ())
#define OTPCLIENT_WINDOW(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), OTPCLIENT_TYPE_WINDOW, OtpclientWindow))
#define OTPCLIENT_IS_WINDOW(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), OTPCLIENT_TYPE_WINDOW))

typedef struct _OtpclientWindow OtpclientWindow;
typedef struct _OtpclientWindowClass OtpclientWindowClass;

GType otpclient_window_get_type (void);

OtpclientWindow *otpclient_window_new (GtkApplication *app,
                                       gint            width,
                                       gint            height,
                                       AppData        *app_data);

G_END_DECLS
