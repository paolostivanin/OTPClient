#pragma once

G_BEGIN_DECLS

#define UI_PARTIAL_PATH         "share/otpclient/otpclient.ui"
#define AP_PARTIAL_PATH         "share/otpclient/add_popover.ui"
#define SP_PARTIAL_PATH         "share/otpclient/settings_popover.ui"

GtkBuilder *get_builder_from_partial_path (const gchar *partial_path);

G_END_DECLS
