#include <gtk/gtk.h>
#include <cotp.h>
#include "db-misc.h"
#include "treeview.h"
#include "otpclient.h"
#include "liststore-misc.h"


void
traverse_liststore (GtkListStore *liststore, UpdateData *kf_data)
{
    GtkTreeIter iter;
    gboolean valid, is_active;
    gchar *acc_name;

    g_return_if_fail (liststore != NULL);

    valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (liststore), &iter);

    while (valid) {
        gtk_tree_model_get (GTK_TREE_MODEL (liststore), &iter,
                            COLUMN_BOOLEAN, &is_active, COLUMN_ACNM, &acc_name, -1);

        if (is_active) {
            set_otp (liststore, iter, acc_name, kf_data);
        }

        g_free (acc_name);

        valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (liststore), &iter);
    }
}


void
set_otp (GtkListStore *list_store, GtkTreeIter iter, gchar *account_name, UpdateData *kf_data)
{
    GError *err = NULL;
    GKeyFile *kf = g_key_file_new ();
    g_key_file_load_from_data (kf, kf_data->in_memory_json, (gsize)-1, G_KEY_FILE_NONE, NULL);
    gchar *secret = g_key_file_get_string (kf, KF_GROUP, account_name, &err);
    cotp_error_t otp_err;
    gchar *totp = get_totp (secret, 6, SHA1, &otp_err);
    if (otp_err == INVALID_B32_INPUT) {
        return;
    }
    gtk_list_store_set (list_store, &iter, COLUMN_OTP, totp, -1);
    g_free (totp);
    g_free (secret);
    g_key_file_free (kf);
}