#pragma once

#include "data.h"

G_BEGIN_DECLS

enum {
    COLUMN_TYPE,
    COLUMN_ACC_LABEL,
    COLUMN_ACC_ISSUER,
    COLUMN_OTP,
    COLUMN_VALIDITY,
    COLUMN_PERIOD,
    COLUMN_UPDATED,
    COLUMN_LESS_THAN_A_MINUTE,
    COLUMN_POSITION_IN_DB,
    NUM_COLUMNS
};

void create_treeview    (AppData            *app_data);

void update_model       (AppData            *app_data);

void row_selected_cb    (GtkTreeView        *tree_view,
                         GtkTreePath        *path,
                         GtkTreeViewColumn  *column,
                         gpointer            user_data);

void reorder_db         (AppData            *app_data);

void regenerate_model   (AppData            *app_data);

G_END_DECLS
