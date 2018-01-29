#pragma once

#include <gtk/gtk.h>
#include "db-misc.h"

G_BEGIN_DECLS

#define ANDOTP_IMPORT_ACTION_NAME "import_andotp"
#define AUTHPLUS_IMPORT_ACTION_NAME "import_authplus"

typedef struct _otp_t {
    gchar *type;

    gchar *algo;

    guint32 digits;

    union {
        guint32 period;
        guint64 counter;
    };

    gchar *label;

    gchar *issuer;

    gchar *secret;
} otp_t;

typedef struct _import_data_t {
    GtkWidget *main_window;
    DatabaseData *db_data;
} ImportData;


void    select_file_cb      (GSimpleAction   *simple,
                             GVariant        *parameter,
                             gpointer         user_data);

GSList *get_authplus_data   (const gchar     *zip_path,
                             const gchar     *password,
                             gint32           max_file_size,
                             GError         **err);

GSList *get_andotp_data     (const gchar     *path,
                             const gchar     *password,
                             gint32           max_file_size,
                             GError         **err);

G_END_DECLS