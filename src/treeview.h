#pragma once

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
    NUM_COLUMNS
};

void create_treeview    (AppData            *app_data);

void update_model       (DatabaseData       *db_data,
                         GtkTreeView        *tree_view);

void delete_rows_cb     (GtkTreeView        *tree_view,
                         GtkTreePath        *path,
                         GtkTreeViewColumn  *column,
                         gpointer            user_data);

void row_selected_cb    (GtkTreeView        *tree_view,
                         GtkTreePath        *path,
                         GtkTreeViewColumn  *column,
                         gpointer            user_data);

G_END_DECLS