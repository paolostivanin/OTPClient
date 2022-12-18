#pragma once

G_BEGIN_DECLS

typedef struct widgets_t {
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

void     manual_add_cb          (GSimpleAction  *simple,
                                 GVariant       *parameter,
                                 gpointer        user_data);

void     manual_add_cb_shortcut (GtkWidget      *w,
                                 gpointer        user_data);

gboolean parse_user_data        (Widgets        *widgets,
                                 DatabaseData   *db_data);

G_END_DECLS