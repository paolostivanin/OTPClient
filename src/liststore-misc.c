#include <gtk/gtk.h>
#include <cotp.h>
#include <json-glib/json-glib.h>
#include "db-misc.h"
#include "treeview.h"
#include "liststore-misc.h"


typedef struct _otp_data {
    gchar *secret;
    gchar *algo;
    gint digits;
} OtpData;

static void set_otp_data (OtpData *otp_data, DatabaseData *db_data, gint row_number);

static void clean_otp_data (OtpData *otp_data);


void
traverse_liststore (GtkListStore *liststore,
                    DatabaseData *db_data)
{
    GtkTreeIter iter;
    gboolean valid, is_active;

    g_return_if_fail (liststore != NULL);

    valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (liststore), &iter);

    while (valid) {
        gtk_tree_model_get (GTK_TREE_MODEL (liststore), &iter,
                            COLUMN_BOOLEAN, &is_active,
                            -1);

        if (is_active) {
            set_otp (liststore, iter, db_data);
        }

        valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (liststore), &iter);
    }
}


void
set_otp (GtkListStore   *list_store,
         GtkTreeIter     iter,
         DatabaseData   *db_data)
{
    OtpData *otp_data = g_new0 (OtpData, 1);

    GtkTreePath *path = gtk_tree_model_get_path (GTK_TREE_MODEL (list_store), &iter);
    gint *row_number = gtk_tree_path_get_indices (path); // starts from 0

    // TODO can be also HOTP (increase the counter after it has been used)
    set_otp_data (otp_data, db_data, row_number[0]);

    gint algo;
    if (g_strcmp0 (otp_data->algo, "SHA1") == 0) {
        algo = SHA1;
    } else if (g_strcmp0 (otp_data->algo, "SHA256") == 0) {
        algo = SHA256;
    } else {
        algo = SHA512;
    }

    cotp_error_t otp_err;
    gchar *totp = get_totp (otp_data->secret, otp_data->digits, algo, &otp_err);
    if (otp_err == INVALID_B32_INPUT) {
        clean_otp_data (otp_data);
        return;
    }
    gtk_list_store_set (list_store, &iter, COLUMN_OTP, totp, -1);

    g_free (totp);
    clean_otp_data (otp_data);
}


static void
set_otp_data (OtpData       *otp_data,
              DatabaseData  *db_data,
              gint           row_number)
{
    JsonArray *ja = json_node_get_array (db_data->json_data);
    JsonObject *jo = json_array_get_object_element (ja, (guint) row_number);
    otp_data->secret = g_strdup (json_object_get_string_member (jo, "secret"));
    otp_data->algo = g_strdup (json_object_get_string_member (jo, "algo"));
    otp_data->digits = (gint) json_object_get_int_member (jo, "digits");
}


static void
clean_otp_data (OtpData *otp_data)
{
    g_free (otp_data->secret);
    g_free (otp_data->algo);
    g_free (otp_data);
}