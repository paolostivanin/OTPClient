#pragma once

#include <gtk/gtk.h>
#include "data.h"

G_BEGIN_DECLS

#define ANDOTP_IMPORT_ACTION_NAME           "import_andotp"
#define ANDOTP_IMPORT_PLAIN_ACTION_NAME     "import_andotp_plain"
#define FREEOTPPLUS_IMPORT_ACTION_NAME      "import_freeotpplus"
#define AEGIS_IMPORT_ACTION_NAME            "import_aegis"
#define AEGIS_IMPORT_ENC_ACTION_NAME        "import_aegis_enc"
#define GOOGLE_MIGRATION_FILE_ACTION_NAME   "import_google_qr_file"
#define GOOGLE_MIGRATION_WEBCAM_ACTION_NAME "import_google_qr_webcam"

typedef struct otp_object_t {
    gchar *type;

    gchar *algo;

    guint32 digits;

    union {
        guint32 period;
        guint64 counter;
    };

    gchar *account_name;

    gchar *issuer;

    gchar *secret;
} otp_t;

void    select_file_cb          (GSimpleAction   *simple,
                                 GVariant        *parameter,
                                 gpointer         user_data);

gchar  *update_db_from_otps     (GSList          *otps,
                                 AppData         *app_data);

void    free_otps_gslist        (GSList          *otps,
                                 guint            list_len);

G_END_DECLS
