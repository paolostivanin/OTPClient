#pragma once

#include <glib.h>
#include "common.h"
#include "import-diagnostics.h"

G_BEGIN_DECLS

GSList *google_migration_decode (const gchar  *uri,
                                guint        *invalid_count,
                                guint        *batch_size,
                                guint        *batch_index,
                                GError      **error);

GSList *google_migration_decode_full (const gchar *uri, guint *invalid_count,
                                      guint *batch_size, guint *batch_index,
                                      OtpImportDiagnostics *diagnostics, GError **error);

G_END_DECLS
