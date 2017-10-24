#pragma once


typedef struct _widgets {
    GtkWidget *dialog;

    GtkWidget *grid;

    GArray *type_cb_box;
    GArray *acc_entry;
    GArray *iss_entry;
    GArray *key_entry;
    GArray *dig_cb_box;
    GArray *alg_cb_box;
    GArray *spin_btn;

    gint grid_top;
} Widgets;


gboolean    parse_user_data (Widgets       *widgets,
                             DatabaseData  *db_data);