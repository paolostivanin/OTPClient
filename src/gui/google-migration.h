#pragma once

#include <glib.h>
#include "common.h"

G_BEGIN_DECLS

GSList *google_migration_decode (const gchar  *uri,
                                guint        *invalid_count,
                                guint        *batch_size,
                                guint        *batch_index,
                                GError      **error);

G_END_DECLS
