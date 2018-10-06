#pragma once

typedef struct _widgets {
    GtkWidget *dialog;
    GtkWidget *otp_cb;
    GtkWidget *algo_cb;
    GtkWidget *steam_ck;
    GtkWidget *label_entry;
    GtkWidget *iss_entry;
    GtkWidget *sec_entry;
    GtkWidget *digits_entry;
    GtkWidget *period_entry;
    GtkWidget *counter_entry;
} Widgets;


gboolean    parse_user_data (Widgets       *widgets,
                             DatabaseData  *db_data);