#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define OTPCLIENT_TYPE_APPLICATION (otpclient_application_get_type())

G_DECLARE_FINAL_TYPE (OTPClientApplication, otpclient_application, OTPCLIENT, APPLICATION, AdwApplication)

OTPClientApplication *otpclient_application_new (void);

G_END_DECLS