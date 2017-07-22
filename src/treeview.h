#pragma once

GtkListStore *create_treeview (GtkWidget *main_window, UpdateData *kf_update_data);

void update_model (UpdateData *kf_data, GtkListStore *store);