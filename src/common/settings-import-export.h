#pragma once

#include <glib.h>

G_BEGIN_DECLS

gchar    *export_settings_to_json   (GError **err);

gboolean  import_settings_from_json (const gchar *json_str,
                                     GError     **err);

G_END_DECLS
