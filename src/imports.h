#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define ANDOTP_BTN_NAME     "andotp_imp_btn"
#define AUTHPLUS_BTN_NAME   "authplus_imp_btn"


typedef struct _otp_t {
    gchar *type;

    gchar *algo;

    guint8 digits;

    union {
        gint16 period;
        gint64 counter;
    };

    gchar *label;

    gchar *issuer;

    gchar *secret;
} otp_t;


void    select_file_cb      (GtkWidget       *btn,
                             gpointer         user_data);

GSList *get_authplus_data   (const gchar     *zip_path,
                             const gchar     *password,
                             GError         **err);

GSList *get_andotp_data     (const gchar     *path,
                             const gchar     *password,
                             GError         **err);

G_END_DECLS