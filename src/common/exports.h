#pragma once

#include <gtk/gtk.h>
#include <jansson.h>

G_BEGIN_DECLS

#define ANDOTP_EXPORT_ACTION_NAME           "export_andotp"
#define ANDOTP_EXPORT_PLAIN_ACTION_NAME     "export_andotp_plain"
#define FREEOTPPLUS_EXPORT_ACTION_NAME      "export_freeotpplus"
#define AEGIS_EXPORT_ACTION_NAME            "export_aegis"
#define AEGIS_EXPORT_PLAIN_ACTION_NAME      "export_aegis_plain"
#define AUTHPRO_EXPORT_PLAIN_ACTION_NAME    "export_authpro"
#define TWOFAS_EXPORT_PLAIN_ACTION_NAME     "export_2fas"


void    export_data_cb     (GSimpleAction   *simple,
                            GVariant        *parameter,
                            gpointer         user_data);

gchar  *export_andotp      (const gchar      *export_path,
                            const gchar      *password,
                            json_t           *json_db_data);

gchar  *export_freeotpplus (const gchar      *export_path,
                            json_t           *json_db_data);

gchar  *export_aegis       (const gchar      *export_path,
                            json_t           *json_db_data,
                            const gchar      *password);

gchar  *export_authpro     (const gchar      *export_path,
                            json_t           *json_db_data,
                            const gchar      *password);

gchar  *export_2fas        (const gchar      *export_path,
                            json_t           *json_db_data,
                            const gchar      *password);

G_END_DECLS
