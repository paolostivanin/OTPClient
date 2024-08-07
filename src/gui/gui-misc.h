#pragma once

#include <gtk/gtk.h>
#include <jansson.h>
#include "data.h"

G_BEGIN_DECLS

void      icon_press_cb                (GtkEntry       *entry,
                                        gint            position,
                                        GdkEventButton *event,
                                        gpointer        data);

guint     get_row_number_from_iter     (GtkListStore   *list_store,
                                        GtkTreeIter     iter);

void      send_ok_cb                   (GtkWidget      *entry,
                                        gpointer        user_data);

gchar    *parse_uris_migration         (AppData        *app_data,
                                        const gchar    *user_uri,
                                        gboolean        google_migration);

gchar    *g_trim_whitespace            (const gchar    *str);

GSList   *decode_migration_data        (const gchar    *encoded_uri);

gchar    *g_uri_unescape_string_secure (const gchar    *escaped_string,
                                        const gchar    *illegal_characters);

guchar   *g_base64_decode_secure       (const gchar    *text,
                                        gsize          *out_len);

void    import_data_cb          (GSimpleAction   *simple,
                                 GVariant        *parameter,
                                 gpointer         user_data);

void    export_data_cb          (GSimpleAction   *simple,
                                 GVariant        *parameter,
                                 gpointer         user_data);

gchar  *update_db_from_otps     (GSList          *otps,
                                 AppData         *app_data);

void    load_new_db              (AppData        *app_data,
                                    GError        **err);

G_END_DECLS
