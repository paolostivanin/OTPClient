#pragma once

#include <glib.h>

G_BEGIN_DECLS

void set_otps_from_uris (const gchar   *otpauth_uris,
                         GSList       **otps);

G_END_DECLS