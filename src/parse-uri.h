#pragma once

#include <glib.h>

void set_otps_from_uris (const gchar   *otpauth_uris,
                         GSList       **otps);