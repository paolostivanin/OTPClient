#pragma once

#include <glib.h>

G_BEGIN_DECLS

void   set_otps_from_uris (const gchar   *otpauth_uris,
                           GSList       **otps);

gchar *get_otpauth_uri    (AppData       *app_data,
                           json_t        *obj);

G_END_DECLS