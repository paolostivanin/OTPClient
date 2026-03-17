#pragma once

#include <adwaita.h>
#include "db-common.h"

G_BEGIN_DECLS

#define DB_INFO_TYPE_DIALOG (db_info_dialog_get_type ())

G_DECLARE_FINAL_TYPE (DbInfoDialog, db_info_dialog, DB_INFO, DIALOG, AdwDialog)

DbInfoDialog *db_info_dialog_new (DatabaseData *db_data);

G_END_DECLS
