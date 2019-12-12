#pragma once

#include <gtk/gtk.h>
#include <jansson.h>

G_BEGIN_DECLS

#define ANDOTP_EXPORT_ACTION_NAME   "export_andotp"
#define ANDOTP_EXPORT_PLAIN_ACTION_NAME   "export_andotp_plain"

void
export_data_cb (GSimpleAction *simple,
                GVariant      *parameter,
                gpointer       user_data);

gchar *
export_andotp ( const gchar *export_path,
                const gchar *password,
                json_t *json_db_data);

G_END_DECLS