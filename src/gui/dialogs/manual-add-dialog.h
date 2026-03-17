#pragma once

#include <adwaita.h>
#include "db-common.h"

G_BEGIN_DECLS

#define MANUAL_ADD_TYPE_DIALOG (manual_add_dialog_get_type ())

G_DECLARE_FINAL_TYPE (ManualAddDialog, manual_add_dialog, MANUAL_ADD, DIALOG, AdwDialog)

typedef void (*ManualAddCallback) (gpointer user_data);

ManualAddDialog *manual_add_dialog_new (DatabaseData      *db_data,
                                        ManualAddCallback  callback,
                                        gpointer           user_data);

G_END_DECLS
