#pragma once

#include <gtk/gtk.h>

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
G_END_DECLS