#include <gtk/gtk.h>
#include <gcrypt.h>
#include <jansson.h>

static void icon_press_cb (GtkEntry *entry, gint position, GdkEventButton *event, gpointer data);


void
set_icon_to_entry (GtkWidget    *entry,
                   const gchar  *icon_name,
                   const gchar  *tooltip_text)
{
    GIcon *gicon = g_themed_icon_new_with_default_fallbacks (icon_name);
    gtk_entry_set_icon_from_gicon (GTK_ENTRY (entry), GTK_ENTRY_ICON_SECONDARY, gicon);
    gtk_entry_set_icon_activatable (GTK_ENTRY (entry), GTK_ENTRY_ICON_SECONDARY, TRUE);
    gtk_entry_set_icon_tooltip_text (GTK_ENTRY (entry), GTK_ENTRY_ICON_SECONDARY, tooltip_text);
    gtk_entry_set_visibility (GTK_ENTRY (entry), FALSE);
    g_signal_connect (entry, "icon-press", G_CALLBACK(icon_press_cb), NULL);
}


static void
icon_press_cb (GtkEntry         *entry,
               gint              position __attribute__((unused)),
               GdkEventButton   *event    __attribute__((unused)),
               gpointer          data     __attribute__((unused)))
{
    gtk_entry_set_visibility (GTK_ENTRY (entry), !gtk_entry_get_visibility (entry));
}


GtkWidget *
create_box_with_buttons (const gchar *add_btn_name,
                         const gchar *del_btn_name)
{
    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class (gtk_widget_get_style_context (box), "linked");

    GtkWidget *add_button = gtk_button_new ();
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


GtkWidget *
find_widget (GtkWidget      *parent,
             const gchar    *widget_name)
{
    GtkWidget *found_widget = NULL;
    GList *children = NULL;
    if (GTK_IS_BIN (parent)) {
        GtkWidget *header_bar = gtk_window_get_titlebar (GTK_WINDOW (parent));
        GList *child = gtk_container_get_children (GTK_CONTAINER (header_bar)); //there's only one child, a GTK_CONTAINER (box)
        children = gtk_container_get_children (GTK_CONTAINER (child->data));
        g_list_free (child);
    } else {
        children = gtk_container_get_children (GTK_CONTAINER (parent)); //children are the 3 buttons
    }

    for (; children != NULL; children = g_list_next (children)) {
        if (g_strcmp0 (gtk_widget_get_name (children->data), widget_name) == 0) {
            found_widget = children->data;
            break;
        }
    }

    g_list_free (children);

    return found_widget;
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


guint32
jenkins_one_at_a_time_hash (const gchar *key, gsize len)
{
    guint32 hash, i;
    for(hash = i = 0; i < len; ++i) {
        hash += key[i];
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);

    return hash;
}


guint32
json_object_get_hash (json_t *obj)
{
    const gchar *key;
    json_t *value;
    gchar *tmp_string = gcry_calloc_secure (256, 1);
    json_object_foreach (obj, key, value) {
        if (g_strcmp0 (key, "period") == 0 || g_strcmp0 (key, "counter") == 0) {
            json_int_t v = json_integer_value (value);
            g_snprintf (tmp_string + strlen (tmp_string), 256, "%ld", (gint64) v);
        } else {
            g_strlcat (tmp_string, json_string_value (value), 256);
        }
    }

    guint32 hash = jenkins_one_at_a_time_hash (tmp_string, strlen (tmp_string) + 1);

    json_decref (obj);
    json_decref (value);
    gcry_free (tmp_string);

    return hash;
}


json_t *
build_json_obj (const gchar *type,
                const gchar *acc_label,
                const gchar *acc_iss,
                const gchar *acc_key,
                gint         digits,
                const gchar *algo,
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
        json_object_set (obj, "period", json_integer (30));
    } else {
        json_object_set (obj, "counter", json_integer (ctr));
    }

    return obj;
}