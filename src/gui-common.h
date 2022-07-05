#pragma once

#include <gtk/gtk.h>
#include <jansson.h>
#include "data.h"

G_BEGIN_DECLS

void    icon_press_cb            (GtkEntry       *entry,
                                  gint            position,
                                  GdkEventButton *event,
                                  gpointer        data);

guint   get_row_number_from_iter (GtkListStore   *list_store,
                                  GtkTreeIter     iter);

json_t *build_json_obj           (const gchar    *type,
                                  const gchar    *acc_label,
                                  const gchar    *acc_iss,
                                  const gchar    *acc_key,
                                  guint           digits,
                                  const gchar    *algo,
                                  guint           period,
                                  guint64         ctr);

void    send_ok_cb               (GtkWidget      *entry,
                                  gpointer        user_data);

gchar  *parse_uris_migration     (AppData        *app_data,
                                  const gchar    *user_uri,
                                  gboolean        google_migration);

G_END_DECLS
