#pragma once

G_BEGIN_DECLS

#define UI_PARTIAL_PATH         "share/otpclient/otpclient.ui"

GtkBuilder *get_builder_from_partial_path (const gchar *partial_path);

G_END_DECLS