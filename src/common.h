#pragma once

#include <gtk/gtk.h>
#include <jansson.h>

G_BEGIN_DECLS

void         set_icon_to_entry          (GtkWidget      *entry,
                                         const gchar    *icon_name,
                                         const gchar    *tooltip_text);

GtkWidget   *create_box_with_buttons    (const gchar    *add_btn_name,
                                         const gchar    *del_btn_name);

GtkWidget   *create_header_bar          (const gchar    *headerbar_title);

GtkWidget   *find_widget                (GtkWidget      *parent,
                                         const gchar    *widget_name);

guint        get_row_number_from_iter   (GtkListStore   *list_store,
                                         GtkTreeIter     iter);

gchar       *secure_strdup              (const gchar    *src);

guint32      jenkins_one_at_a_time_hash (const gchar    *key,
                                         gsize           len);

guint32      json_object_get_hash       (json_t *obj);

json_t      *build_json_obj             (const gchar *type,
                                         const gchar *acc_label,
                                         const gchar *acc_iss,
                                         const gchar *acc_key,
                                         gint         digits,
                                         const gchar *algo,
                                         gint64       ctr);

G_END_DECLS