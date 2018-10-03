#include <gtk/gtk.h>
#include <cotp.h>
#include <jansson.h>
#include <app.h>
#include "db-misc.h"
#include "treeview.h"
#include "liststore-misc.h"
#include "gquarks.h"
#include "common.h"


typedef struct _otp_data {
    gchar *type;
    gchar *secret;
    gchar *algo;
    gint digits;
    gint64 counter;
    gboolean steam;
} OtpData;

static void set_otp_data (OtpData *otp_data, DatabaseData *db_data, guint row_number);

static gboolean foreach_func_update_otps (GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, gpointer user_data);

static void clean_otp_data (OtpData *otp_data);


gboolean
traverse_listore (gpointer user_data)
{
    AppData *app_data = (AppData *)user_data;
    gtk_tree_model_foreach (GTK_TREE_MODEL(gtk_tree_view_get_model (app_data->tree_view)), foreach_func_update_otps, app_data);

    return TRUE;
}


void
set_otp (GtkListStore   *list_store,
         GtkTreeIter     iter,
         DatabaseData   *db_data)
{
    OtpData *otp_data = g_new0 (OtpData, 1);

    guint row_number = get_row_number_from_iter (list_store, iter);

    set_otp_data (otp_data, db_data, row_number);

    gint algo;
    if (g_strcmp0 (otp_data->algo, "SHA1") == 0) {
        algo = SHA1;
    } else if (g_strcmp0 (otp_data->algo, "SHA256") == 0) {
        algo = SHA256;
    } else {
        algo = SHA512;
    }

    cotp_error_t otp_err;
    gchar *otp;
    if (g_strcmp0 (otp_data->type, "TOTP") == 0) {
        if (otp_data->steam) {
            otp = get_steam_totp (otp_data->secret, &otp_err);
        } else {
            otp = get_totp (otp_data->secret, otp_data->digits, algo, &otp_err);
        }
    } else {
        // clean previous HOTP info
        g_free (db_data->last_hotp);
        g_date_time_unref (db_data->last_hotp_update);

        otp = get_hotp (otp_data->secret, otp_data->counter, otp_data->digits, algo, &otp_err);

        db_data->last_hotp = g_strdup (otp);
        db_data->last_hotp_update = g_date_time_new_now_local ();
    }
    if (otp_err == INVALID_B32_INPUT) {
        clean_otp_data (otp_data);
        return;
    }
    gtk_list_store_set (list_store, &iter, COLUMN_OTP, otp, -1);

    g_free (otp);
    clean_otp_data (otp_data);
}


static gboolean
foreach_func_update_otps (GtkTreeModel *model,
                          GtkTreePath  *path,
                          GtkTreeIter  *iter,
                          gpointer      user_data)
{
    AppData *app_data = (AppData *)user_data;
    gchar *otp_type, *otp;
    guint validity, period;
    gboolean only_a_minute_left, updated;

    gtk_tree_model_get (model, iter,
                        COLUMN_TYPE, &otp_type,
                        COLUMN_OTP, &otp,
                        COLUMN_VALIDITY, &validity,
                        COLUMN_PERIOD, &period,
                        COLUMN_UPDATED, &updated,
                        COLUMN_LESS_THAN_A_MINUTE, &only_a_minute_left,
                        -1);

    if (otp != NULL && g_utf8_strlen (otp, -1) > 4 && g_strcmp0 (otp_type, "TOTP") == 0) {
        gboolean short_countdown = (period <= 60 || only_a_minute_left) ? TRUE : FALSE;
        gint remaining_seconds = (!short_countdown ? 119 : 59) - g_date_time_get_second (g_date_time_new_now_local());
        gint token_validity = remaining_seconds % period;
        if (remaining_seconds % period == 60) {
            short_countdown = TRUE;
        }
        if (remaining_seconds % period == 0) {
            if (!app_data->show_next_otp || updated) {
                short_countdown = FALSE;
                updated = FALSE;
                gtk_list_store_set (GTK_LIST_STORE (model), &iter, COLUMN_OTP, "");
            } else {
                updated = TRUE;
                set_otp (GTK_LIST_STORE (model), *iter, app_data->db_data);
            }
        }
        gtk_list_store_set (GTK_LIST_STORE (model), &iter, COLUMN_VALIDITY, token_validity, COLUMN_UPDATED, updated, COLUMN_LESS_THAN_A_MINUTE, short_countdown, -1);
    }

    g_free (otp_type);
    g_free (otp);

    // do not stop walking the store, check next row
    return FALSE;
}


static void
set_otp_data (OtpData       *otp_data,
              DatabaseData  *db_data,
              guint          row_number)
{
    json_t *obj = json_array_get (db_data->json_data, row_number);

    otp_data->type = g_strdup (json_string_value (json_object_get (obj, "type")));
    otp_data->secret = secure_strdup (json_string_value (json_object_get (obj, "secret")));
    otp_data->algo = g_strdup (json_string_value (json_object_get (obj, "algo")));
    otp_data->digits = (gint)json_integer_value (json_object_get (obj, "digits"));
    otp_data->steam = (g_ascii_strcasecmp (json_object_get (obj, "issuer"), "steam") == 0 ? TRUE : FALSE);
    if (json_object_get (obj, "counter") != NULL) {
        GError *err = NULL;
        otp_data->counter = json_integer_value (json_object_get (obj, "counter"));
        // every time HOTP is accessed, counter must be increased
        json_object_set (obj, "counter", json_integer (otp_data->counter + 1));
        update_and_reload_db (db_data, NULL, FALSE, &err);
        if (err != NULL && !g_error_matches (err, missing_file_gquark (), MISSING_FILE_CODE)) {
            g_printerr ("%s\n", err->message);
        }
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