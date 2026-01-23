#pragma once

#include <gtk/gtk.h>
#include "data.h"

G_BEGIN_DECLS

#define OTPCLIENT_TYPE_APPLICATION (otpclient_application_get_type ())
#define OTPCLIENT_APPLICATION(obj) (G_TYPE_CHECK_INSTANCE_CAST ((obj), OTPCLIENT_TYPE_APPLICATION, OtpclientApplication))
#define OTPCLIENT_IS_APPLICATION(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), OTPCLIENT_TYPE_APPLICATION))

typedef struct _OtpclientApplication OtpclientApplication;
typedef struct _OtpclientApplicationClass OtpclientApplicationClass;

GType otpclient_application_get_type (void);

OtpclientApplication *otpclient_application_new (void);

void otpclient_application_clear_app_data (OtpclientApplication *app);

G_END_DECLS
