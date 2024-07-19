#pragma once

#include <glib.h>

G_BEGIN_DECLS

void    set_otps_from_uris (const gchar  *otpauth_uris,
                            GSList      **otps);

gchar  *get_otpauth_uri    (json_t       *obj);

GSList *get_otpauth_data   (const gchar  *path,
                            gint32        max_file_size,
                            GError      **err);

G_END_DECLS