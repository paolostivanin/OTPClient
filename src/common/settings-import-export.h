#pragma once

#include <glib.h>

G_BEGIN_DECLS

gchar    *export_settings_to_json   (GError **err);

/* Three of the exportable keys only describe an intent: the login-time entry
 * and the background grant they stand for live with the desktop, and writing
 * the key does not touch either. out_touched_startup, when not NULL, is set
 * TRUE if the imported JSON carried any of them, so the caller can put the
 * desktop side right (the GUI) or say that it cannot (the CLI). */
gboolean  import_settings_from_json (const gchar *json_str,
                                     gboolean    *out_touched_startup,
                                     GError     **err);

G_END_DECLS
