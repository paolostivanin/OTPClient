#pragma once

#include <adwaita.h>
#include "db-common.h"

G_BEGIN_DECLS

#define KDF_TYPE_DIALOG (kdf_dialog_get_type ())

G_DECLARE_FINAL_TYPE (KdfDialog, kdf_dialog, KDF, DIALOG, AdwDialog)

KdfDialog *kdf_dialog_new (DatabaseData *db_data);

G_END_DECLS
