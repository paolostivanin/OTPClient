#pragma once

#include "glib.h"

G_BEGIN_DECLS

gint32 get_max_file_size_from_memlock (void);

gchar *init_libs (gint32 max_file_size);

G_END_DECLS