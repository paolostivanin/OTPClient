#pragma once

G_BEGIN_DECLS

enum {
    COLUMN_BOOLEAN,
    COLUMN_TYPE,
    COLUMN_ACC_LABEL,
    COLUMN_ACC_ISSUER,
    COLUMN_OTP,
    NUM_COLUMNS
};

GtkListStore    *create_treeview    (GtkWidget    *main_window,
                                     DatabaseData *db_data);

void             update_model       (DatabaseData *db_data,
                                     GtkListStore *store);

G_END_DECLS