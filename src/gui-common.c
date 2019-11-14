#include <gtk/gtk.h>
#include <gcrypt.h>
#include <jansson.h>


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
    GtkTreePath *path = gtk_tree_model_get_path (GTK_TREE_MODEL (list_store), &iter);
    gint *row_number = gtk_tree_path_get_indices (path); // starts from 0
    guint row = (guint) row_number[0];
    gtk_tree_path_free (path);

    return row;
}


gchar *
secure_strdup (const gchar *src)
{
    gchar *sec_buf = gcry_calloc_secure (strlen (src) + 1, 1);
    memcpy (sec_buf, src, strlen (src) + 1);

    return sec_buf;
}


json_t *
build_json_obj (const gchar *type,
                const gchar *acc_label,
                const gchar *acc_iss,
                const gchar *acc_key,
                gint         digits,
                const gchar *algo,
                gint         period,
                gint64       ctr)
{
    json_t *obj = json_object ();
    json_object_set (obj, "type", json_string (type));
    json_object_set (obj, "label", json_string (acc_label));
    json_object_set (obj, "issuer", json_string (acc_iss));
    json_object_set (obj, "secret", json_string (acc_key));
    json_object_set (obj, "digits", json_integer (digits));
    json_object_set (obj, "algo", json_string (algo));

    json_object_set (obj, "secret", json_string (acc_key));

    if (g_strcmp0 (type, "TOTP") == 0) {
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
