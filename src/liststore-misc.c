#include <gtk/gtk.h>
#include <cotp.h>
#include <jansson.h>
#include "treeview.h"
#include "liststore-misc.h"
#include "gquarks.h"
#include "gui-common.h"
#include "common/common.h"


typedef struct _otp_data {
    gchar *type;
    gchar *secret;
    gchar *algo;
    gint digits;
    gint period;
    gint64 counter;
    gboolean steam;
} OtpData;

static void set_otp_data (OtpData *otp_data, AppData *app_data, guint row_number);

static gboolean foreach_func_update_otps (GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer user_data);

static void clean_otp_data (OtpData *otp_data);


gboolean
traverse_liststore (gpointer user_data)
{
    AppData *app_data = (AppData *)user_data;
    gtk_tree_model_foreach (GTK_TREE_MODEL(gtk_tree_view_get_model (app_data->tree_view)), foreach_func_update_otps, app_data);

    return TRUE;
}


void
set_otp (GtkListStore   *list_store,
         GtkTreeIter     iter,
         AppData        *app_data)
{
    OtpData *otp_data = g_new0 (OtpData, 1);

    guint row_number = get_row_number_from_iter (list_store, iter);

    set_otp_data (otp_data, app_data, row_number);

    gint algo = get_algo_int_from_str (otp_data->algo);

    cotp_error_t otp_err;
    gchar *otp;
    if (g_strcmp0 (otp_data->type, "TOTP") == 0) {
        if (otp_data->steam) {
            otp = get_steam_totp (otp_data->secret, otp_data->period, &otp_err);
        } else {
            otp = get_totp (otp_data->secret, otp_data->digits, otp_data->period, algo, &otp_err);
        }
    } else {
        // clean previous HOTP info
        g_free (app_data->db_data->last_hotp);
        g_date_time_unref (app_data->db_data->last_hotp_update);

        otp = get_hotp (otp_data->secret, otp_data->counter, otp_data->digits, algo, &otp_err);

        app_data->db_data->last_hotp = g_strdup (otp);
        app_data->db_data->last_hotp_update = g_date_time_new_now_local ();
    }
    if (otp_err == INVALID_B32_INPUT) {
        clean_otp_data (otp_data);
        return;
    }
    gtk_list_store_set (list_store, &iter, COLUMN_OTP, otp, -1);
    app_data->last_user_activity = g_date_time_new_now_local ();

    g_free (otp);
    clean_otp_data (otp_data);
}


static gboolean
foreach_func_update_otps (GtkTreeModel *model,
                          GtkTreePath  *path    __attribute__((unused)),
                          GtkTreeIter  *iter,
                          gpointer      user_data)
{
    AppData *app_data = (AppData *)user_data;
    gchar *otp_type, *otp;
    guint validity, period;
    gboolean only_a_minute_left, already_updated_once;

    gtk_tree_model_get (model, iter,
                        COLUMN_TYPE, &otp_type,
                        COLUMN_OTP, &otp,
                        COLUMN_VALIDITY, &validity,
                        COLUMN_PERIOD, &period,
                        COLUMN_UPDATED, &already_updated_once,
                        COLUMN_LESS_THAN_A_MINUTE, &only_a_minute_left,
                        -1);

    if (otp != NULL && g_utf8_strlen (otp, -1) > 4 && g_strcmp0 (otp_type, "TOTP") == 0) {
        gboolean short_countdown = (period <= 60 || only_a_minute_left) ? TRUE : FALSE;
        gint remaining_seconds = (!short_countdown ? 119 : 59) - g_date_time_get_second (g_date_time_new_now_local());
        gint token_validity = remaining_seconds % period;
        if (remaining_seconds % period == 60) {
            short_countdown = TRUE;
        }
        if ((remaining_seconds % period) == (period - 1)) {
            if ((app_data->show_next_otp) && (already_updated_once == FALSE)) {
                already_updated_once = TRUE;
                set_otp (GTK_LIST_STORE (model), *iter, app_data);
            } else {
                short_countdown = FALSE;
                already_updated_once = FALSE;
                token_validity = 0;
                gtk_list_store_set (GTK_LIST_STORE (model), iter, COLUMN_OTP, "", -1);
            }
        }
        gtk_list_store_set (GTK_LIST_STORE (model), iter, COLUMN_VALIDITY, token_validity, COLUMN_UPDATED, already_updated_once, COLUMN_LESS_THAN_A_MINUTE, short_countdown, -1);
    }

    g_free (otp_type);
    g_free (otp);

    // do not stop walking the store, check next row
    return FALSE;
}


static void
set_otp_data (OtpData  *otp_data,
              AppData  *app_data,
              guint     row_number)
{
    json_t *obj = json_array_get (app_data->db_data->json_data, row_number);

    otp_data->type = g_strdup (json_string_value (json_object_get (obj, "type")));
    otp_data->secret = secure_strdup (json_string_value (json_object_get (obj, "secret")));
    otp_data->algo = g_strdup (json_string_value (json_object_get (obj, "algo")));
    otp_data->digits = (gint)json_integer_value (json_object_get (obj, "digits"));
    const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
    otp_data->steam = ((issuer != NULL && g_ascii_strcasecmp (issuer, "steam") == 0) ? TRUE : FALSE);
    if (json_object_get (obj, "counter") != NULL) {
        GError *err = NULL;
        otp_data->counter = json_integer_value (json_object_get (obj, "counter"));
        // every time HOTP is accessed, counter must be increased
        json_object_set (obj, "counter", json_integer (otp_data->counter + 1));
        update_and_reload_db (app_data, app_data->db_data, FALSE, &err);
        if (err != NULL && !g_error_matches (err, missing_file_gquark (), MISSING_FILE_CODE)) {
            g_printerr ("%s\n", err->message);
        }
    } else {
        otp_data->period = (gint)json_integer_value (json_object_get (obj, "period"));
    }
}


static void
clean_otp_data (OtpData *otp_data)
{
    g_free (otp_data->type);
    gcry_free (otp_data->secret);
    g_free (otp_data->algo);
    g_free (otp_data);
}
