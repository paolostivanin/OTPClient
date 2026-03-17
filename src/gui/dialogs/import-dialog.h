#pragma once

#include <adwaita.h>
#include "db-common.h"

G_BEGIN_DECLS

#define IMPORT_TYPE_DIALOG (import_dialog_get_type ())

G_DECLARE_FINAL_TYPE (ImportDialog, import_dialog, IMPORT, DIALOG, AdwDialog)

typedef void (*ImportCallback) (gpointer user_data);

ImportDialog *import_dialog_new (DatabaseData   *db_data,
                                 GtkWidget      *parent,
                                 ImportCallback  callback,
                                 gpointer        user_data);

G_END_DECLS
