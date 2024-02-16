#pragma once

#include "otpclient-types.h"
#include <adwaita.h>

G_BEGIN_DECLS

#define OTPCLIENT_TYPE_WINDOW (otpclient_window_get_type())

G_DECLARE_FINAL_TYPE (OTPClientWindow, otpclient_window, OTPCLIENT, WINDOW, AdwApplicationWindow)

GtkWidget *otpclient_window_new (OTPClientApplication *application);

G_END_DECLS