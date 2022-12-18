#include <gtk/gtk.h>
#include <jansson.h>
#include "message-dialogs.h"
#include "add-common.h"
#include "common/common.h"


void
icon_press_cb (GtkEntry         *entry,
               gint              position __attribute__((unused)),
               GdkEventButton   *event    __attribute__((unused)),
               gpointer          data     __attribute__((unused)))
{
    gtk_entry_set_visibility (GTK_ENTRY (entry), !gtk_entry_get_visibility (entry));
}


guint
get_row_number_from_iter (GtkListStore *list_store,
                          GtkTreeIter iter)
{
    GtkTreePath *path = gtk_tree_model_get_path (GTK_TREE_MODEL(list_store), &iter);
    gint *row_number = gtk_tree_path_get_indices (path); // starts from 0
    guint row = (guint)row_number[0];
    gtk_tree_path_free (path);

    return row;
}


json_t *
build_json_obj (const gchar *type,
                const gchar *acc_label,
                const gchar *acc_iss,
                const gchar *acc_key,
                guint        digits,
                const gchar *algo,
                guint        period,
                guint64      ctr)
{
    json_t *obj = json_object ();
    json_object_set (obj, "type", json_string (type));
    json_object_set (obj, "label", json_string (acc_label));
    json_object_set (obj, "issuer", json_string (acc_iss));
    json_object_set (obj, "secret", json_string (acc_key));
    json_object_set (obj, "digits", json_integer (digits));
    json_object_set (obj, "algo", json_string (algo));

    json_object_set (obj, "secret", json_string (acc_key));

    if (g_ascii_strcasecmp (type, "TOTP") == 0) {
        json_object_set (obj, "period", json_integer (period));
    } else {
        json_object_set (obj, "counter", json_integer (ctr));
    }

    return obj;
}


void
send_ok_cb (GtkWidget *entry,
            gpointer   user_data __attribute__((unused)))
{
    gtk_dialog_response (GTK_DIALOG(gtk_widget_get_toplevel (entry)), GTK_RESPONSE_OK);
}


gchar *
parse_uris_migration (AppData  *app_data,
                      const     gchar *user_uri,
                      gboolean  google_migration)
{
    gchar *return_err_msg = NULL;
    GSList *otpauth_decoded_uris = NULL;
    if (google_migration == TRUE) {
        gint failed = 0;
        otpauth_decoded_uris = decode_migration_data (user_uri);
        for (gint i = 0; i < g_slist_length (otpauth_decoded_uris); i++) {
            gchar *uri = g_slist_nth_data (otpauth_decoded_uris, i);
            gchar *err_msg = add_data_to_db (uri, app_data);
            if (err_msg != NULL) {
                failed++;
                g_free (err_msg);
            }
        }
        if (failed > 0) {
            GString *e_msg = g_string_new (NULL);
            g_string_printf (e_msg, "Failed to add all OTPs. Only %u out of %u were successfully added.", g_slist_length (otpauth_decoded_uris) - failed,
                             g_slist_length (otpauth_decoded_uris));
            return_err_msg = g_strdup (e_msg->str);
            g_string_free (e_msg, TRUE);
        }
        g_slist_free_full (otpauth_decoded_uris, g_free);
    } else {
        return_err_msg = add_data_to_db (user_uri, app_data);
    }

    return return_err_msg;
}