#include <gtk/gtk.h>
#include <cotp.h>
#include <jansson.h>
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
    gint period;
    gint64 counter;
} OtpData;

static void set_otp_data (OtpData *otp_data, DatabaseData *db_data, guint row_number);

static void clean_otp_data (OtpData *otp_data);


void
traverse_liststore (GtkListStore *liststore,
                    DatabaseData *db_data)
{
    GtkTreeIter iter;
    gboolean valid, is_active;
    gchar *otp_type;

    g_return_if_fail (liststore != NULL);

    valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (liststore), &iter);

    while (valid) {
        gtk_tree_model_get (GTK_TREE_MODEL (liststore), &iter, COLUMN_BOOLEAN, &is_active, -1);
        gtk_tree_model_get (GTK_TREE_MODEL (liststore), &iter, COLUMN_TYPE, &otp_type, -1);

        if (is_active && g_strcmp0 (otp_type, "TOTP") == 0) {
            set_otp (liststore, iter, db_data);
        }

        valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (liststore), &iter);
        g_free (otp_type);
    }
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
        otp = get_totp (otp_data->secret, otp_data->digits, otp_data->period, algo, &otp_err);
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
    otp_data->period = (gint)json_integer_value (json_object_get (obj, "period"));
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