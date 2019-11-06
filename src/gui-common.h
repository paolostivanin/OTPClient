#pragma once

#include <gtk/gtk.h>
#include <jansson.h>

G_BEGIN_DECLS

void         icon_press_cb              (GtkEntry       *entry,
                                         gint            position,
                                         GdkEventButton *event,
                                         gpointer        data);

GtkWidget   *create_box_with_buttons    (const gchar    *add_btn_name,
                                         const gchar    *del_btn_name,
                                         gboolean        add_btn_is_menu);

GtkWidget   *create_header_bar          (const gchar    *headerbar_title);

guint        get_row_number_from_iter   (GtkListStore   *list_store,
                                         GtkTreeIter     iter);

gchar       *secure_strdup              (const gchar    *src);

json_t      *build_json_obj             (const gchar *type,
                                         const gchar *acc_label,
                                         const gchar *acc_iss,
                                         const gchar *acc_key,
                                         gint         digits,
                                         const gchar *algo,
                                         gint         period,
                                         gint64       ctr);

void send_ok_cb                         (GtkWidget *entry,
                                         gpointer   user_data);

G_END_DECLS