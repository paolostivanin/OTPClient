#pragma once

#include <glib.h>

G_BEGIN_DECLS

gint32  get_max_file_size_from_memlock  (void);

gchar  *init_libs                       (gint32 max_file_size);

gint    get_algo_int_from_str           (const gchar *algo);

G_END_DECLS