#pragma once

#include <jansson.h>

G_BEGIN_DECLS

#define ANDOTP_PLAIN_ACTION_NAME        "andotp_plain"
#define ANDOTP_ENC_ACTION_NAME          "andotp_encrypted"
#define FREEOTPPLUS_PLAIN_ACTION_NAME   "freeotpplus_plain"
#define AEGIS_PLAIN_ACTION_NAME         "aegis_plain"
#define AEGIS_ENC_ACTION_NAME           "aegis_encrypted"
#define AUTHPRO_PLAIN_ACTION_NAME       "authpro_plain"
#define AUTHPRO_ENC_ACTION_NAME         "authpro_encrypted"
#define TWOFAS_PLAIN_ACTION_NAME        "twofas_plain"
#define TWOFAS_ENC_ACTION_NAME          "twofas_encrypted"
#define GOOGLE_FILE_ACTION_NAME         "import_google_qr_file"
#define GOOGLE_WEBCAM_ACTION_NAME       "import_google_qr_webcam"

GSList *get_data_from_provider (const gchar  *action_name,
                                const gchar  *filename,
                                const gchar  *pwd,
                                gint32        max_file_size_from_memlock,
                                GError      **err);

gchar  *export_andotp          (const gchar      *export_path,
                                const gchar      *password,
                                json_t           *json_db_data);

gchar  *export_freeotpplus     (const gchar      *export_path,
                                json_t           *json_db_data);

gchar  *export_aegis       (const gchar      *export_path,
                            const gchar      *password,
                            json_t           *json_db_data);

gchar  *export_authpro     (const gchar      *export_path,
                            const gchar      *password,
                            json_t           *json_db_data);

gchar  *export_twofas      (const gchar      *export_path,
                            const gchar      *password,
                            json_t           *json_db_data);

G_END_DECLS
