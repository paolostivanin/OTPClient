#pragma once

enum {
    COLUMN_BOOLEAN,
    COLUMN_ACNM,
    COLUMN_OTP,
    NUM_COLUMNS
};

GtkListStore *create_treeview (GtkWidget *main_window, UpdateData *kf_update_data);

void update_model (UpdateData *kf_data, GtkListStore *store);