#include <gtk/gtk.h>
#include <string.h>
#include <zbar.h>
#include "app.h"
#include "imports.h"
#include "common.h"
#include "parse-uri.h"

static gchar *check_params (GSList *otps);


gchar *
add_data_to_db (const gchar *otp_uri,
                AppData     *app_data)
{
    GtkListStore *list_store = g_object_get_data (G_OBJECT (app_data->main_window), "lstore");
    GSList *otps = NULL;
    set_otps_from_uris (otp_uri, &otps);
    if (g_slist_length (otps) != 1) {
        return g_strdup ("No valid otpauth uris found");
    }

    gchar *err_msg = check_params (otps);
    if (err_msg != NULL){
        return err_msg;
    }

    err_msg = update_db_from_otps (otps, app_data->db_data, list_store);
    if (err_msg != NULL) {
        return err_msg;
    }

    free_otps_gslist (otps, g_slist_length (otps));

    return NULL;
}


static gchar *
check_params (GSList *otps)
{
    otp_t *otp = g_slist_nth_data (otps, 0);
    if (otp->label == NULL) {
        return g_strdup ("Label can not be empty, otp not imported");
    }

    if (strlen (otp->secret) == 0) {
        return g_strdup ("Secret can not be empty, otp not imported");
    }

    if (g_strcasecmp (otp->type, "TOTP") == 0) {
        if (otp->period < 10 || otp->period > 120) {
            gchar *msg = g_strconcat("[INFO]: invalid period for '", otp->label, "'. Defaulting back to 30 seconds.\n", NULL);
            g_printerr (msg);
            otp->period = 30;
        }
    }

    return NULL;
}