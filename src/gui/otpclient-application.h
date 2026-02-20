#pragma once

#include <gtk/gtk.h>
#include "data.h"

G_BEGIN_DECLS

#define OTPCLIENT_TYPE_APPLICATION (otpclient_application_get_type ())

G_DECLARE_FINAL_TYPE (OtpclientApplication, otpclient_application,
                      OTPCLIENT, APPLICATION, GtkApplication)

OtpclientApplication *otpclient_application_new            (void);

void                  otpclient_application_clear_app_data (OtpclientApplication *app);

G_END_DECLS
