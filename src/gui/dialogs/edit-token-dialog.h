#pragma once

#include <adwaita.h>
#include "db-common.h"

G_BEGIN_DECLS

#define EDIT_TOKEN_TYPE_DIALOG (edit_token_dialog_get_type ())

G_DECLARE_FINAL_TYPE (EditTokenDialog, edit_token_dialog, EDIT_TOKEN, DIALOG, AdwDialog)

typedef void (*EditTokenCallback) (gpointer user_data);

EditTokenDialog *edit_token_dialog_new (json_t            *token_obj,
                                        guint              token_index,
                                        DatabaseData      *db_data,
                                        EditTokenCallback  callback,
                                        gpointer           user_data);

G_END_DECLS
