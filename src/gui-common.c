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


GtkWidget *
create_box_with_buttons (const gchar *add_btn_name,
                         const gchar *del_btn_name,
                         gboolean add_btn_is_menu)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class (gtk_widget_get_style_context (box), "linked");

    GtkWidget *add_button;
    if (add_btn_is_menu) {
        add_button = gtk_menu_button_new ();
    } else {
        add_button = gtk_button_new ();
    }
    gtk_widget_set_name (add_button, add_btn_name);
    gtk_container_add (GTK_CONTAINER (add_button), gtk_image_new_from_icon_name ("list-add-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_container_add (GTK_CONTAINER (box), add_button);
    gtk_widget_set_tooltip_text (add_button, "Add");

    GtkWidget *del_button = gtk_button_new ();
    gtk_widget_set_name (del_button, del_btn_name);
    gtk_container_add (GTK_CONTAINER (del_button), gtk_image_new_from_icon_name ("list-remove-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_container_add (GTK_CONTAINER (box), del_button);
    gtk_widget_set_tooltip_text (del_button, "Remove");

    return box;
}


GtkWidget *
create_header_bar (const gchar *headerbar_title)
{
    GtkWidget *header_bar = gtk_header_bar_new ();
    gtk_header_bar_set_show_close_button (GTK_HEADER_BAR (header_bar), TRUE);
    gtk_header_bar_set_title (GTK_HEADER_BAR (header_bar), headerbar_title);
    gtk_header_bar_set_has_subtitle (GTK_HEADER_BAR (header_bar), FALSE);

    return header_bar;
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
