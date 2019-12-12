#pragma once

#include <glib.h>
#include <jansson.h>

G_BEGIN_DECLS

gint32      get_max_file_size_from_memlock  (void);

gchar      *init_libs                       (gint32 max_file_size);

gint        get_algo_int_from_str           (const gchar *algo);

guint32     jenkins_one_at_a_time_hash      (const gchar    *key,
                                             gsize           len);

guint32     json_object_get_hash            (json_t *obj);

G_END_DECLS