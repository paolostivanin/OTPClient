#pragma once

#include <adwaita.h>
#include "db-common.h"

G_BEGIN_DECLS

#define EXPORT_TYPE_DIALOG (export_dialog_get_type ())

G_DECLARE_FINAL_TYPE (ExportDialog, export_dialog, EXPORT, DIALOG, AdwDialog)

ExportDialog *export_dialog_new (DatabaseData *db_data,
                                  GtkWidget    *parent);
ExportDialog *export_dialog_new_selected (DatabaseData *db_data,
                                           GtkWidget    *parent,
                                           json_t       *tokens);

G_END_DECLS
