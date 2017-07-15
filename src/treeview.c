#include <gtk/gtk.h>
#include "otpclient.h"
#include "kf-misc.h"

static GtkWidget *get_button_box (void);
static gchar **get_account_names (const gchar *dec_kf);
static GtkTreeModel *create_model (gchar **account_names);
static void add_columns (GtkTreeView *treeview);

enum {
    COLUMN_BOOLEAN,
    COLUMN_ACNM,
    COLUMN_OTP,
    NUM_COLUMNS
};


GtkWidget *
create_scrolled_window_with_treeview (GtkWidget *main_win, UpdateData *kf_update_data)
{
    GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_add (GTK_CONTAINER (main_win), vbox);

    GtkWidget *sw = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (sw), GTK_SHADOW_ETCHED_IN);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start (GTK_BOX (vbox), sw, TRUE, TRUE, 0);

    GtkWidget *button_box = get_button_box ();
    gtk_box_pack_end (GTK_BOX(vbox), button_box, FALSE, FALSE, 0);

    gchar **account_names = get_account_names (kf_update_data->in_memory_kf);
    GtkTreeModel *model = create_model (account_names);
    g_strfreev (account_names);

    GtkWidget *treeview = gtk_tree_view_new_with_model (model);
    gtk_tree_view_set_search_column (GTK_TREE_VIEW (treeview), COLUMN_ACNM);

    g_object_unref (model);

    gtk_container_add (GTK_CONTAINER (sw), treeview);

    add_columns (GTK_TREE_VIEW (treeview));

    return sw;
}


static GtkWidget *
get_button_box()
{
    GtkWidget *button_box = gtk_button_box_new (GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout (GTK_BUTTON_BOX (button_box), GTK_BUTTONBOX_END);
    GtkWidget *add_button = gtk_button_new_with_label ("Add");
    gtk_widget_set_name(add_button, "add_btn");
    GtkWidget *remove_button = gtk_button_new_with_label ("Remove");
    gtk_widget_set_name(add_button, "remove_btn");
    // TODO here I need a dialog with dynamic entries "account name" and "key". Then I pass all the data into the struct update and send to update_kf
    g_signal_connect (add_button, "clicked", G_CALLBACK (), NULL); //TODO struct instead of null
    // TODO get the active ticks from treeview and send to update_kf with delete
    g_signal_connect (remove_button, "clicked", G_CALLBACK (), NULL); //TODO struct instead of null
    gtk_container_add (GTK_CONTAINER (button_box), add_button);
    gtk_container_add (GTK_CONTAINER (button_box), remove_button);

    return button_box;
}


static gchar **
get_account_names (const gchar *dec_kf)
{
    GKeyFile * kf = g_key_file_new ();
    g_key_file_load_from_data (kf, dec_kf, (gsize)-1, G_KEY_FILE_NONE, NULL);

    gchar **account_names = g_key_file_get_keys (kf, KF_GROUP, NULL, NULL);

    g_key_file_free (kf);

    return account_names;
}


static GtkTreeModel *
create_model (gchar **account_names)
{
    GtkListStore *store;
    GtkTreeIter iter;

    store = gtk_list_store_new (NUM_COLUMNS, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING);

    gint i = 0;
    while (g_strcmp0 (account_names[i], NULL) != 0) {
        gtk_list_store_append (store, &iter);
        gtk_list_store_set (store, &iter, COLUMN_BOOLEAN, FALSE, COLUMN_ACNM, account_names[i], COLUMN_OTP, "", -1);
        i++;
    }

    return GTK_TREE_MODEL (store);
}


static void
fixed_toggled (GtkCellRendererToggle *cell, gchar *path_str, gpointer data)
{
    // TODO refactor
    GtkTreeModel *model = (GtkTreeModel *)data;
    GtkTreeIter  iter;
    GtkTreePath *path = gtk_tree_path_new_from_string (path_str);

    // set TOTP/HOTP if toggle is active, otherwise remove it

    gtk_tree_path_free (path);
}


static void
add_columns (GtkTreeView *treeview)
{
    GtkCellRenderer *renderer;
    GtkTreeViewColumn *column;
    GtkTreeModel *model = gtk_tree_view_get_model (treeview);

    renderer = gtk_cell_renderer_toggle_new ();
    g_signal_connect (renderer, "toggled", G_CALLBACK (fixed_toggled), model);

    column = gtk_tree_view_column_new_with_attributes ("Show", renderer, "active", COLUMN_BOOLEAN, NULL);
    gtk_tree_view_column_set_sizing (GTK_TREE_VIEW_COLUMN (column), GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width (GTK_TREE_VIEW_COLUMN (column), 50);
    gtk_tree_view_append_column (treeview, column);

    renderer = gtk_cell_renderer_text_new ();
    column = gtk_tree_view_column_new_with_attributes ("Account Name", renderer, "text", COLUMN_ACNM, NULL);
    gtk_tree_view_column_set_sort_column_id (column, COLUMN_ACNM);
    gtk_tree_view_append_column (treeview, column);

    renderer = gtk_cell_renderer_text_new ();
    column = gtk_tree_view_column_new_with_attributes ("OTP Value", renderer, "text", COLUMN_OTP, NULL);
    gtk_tree_view_append_column (treeview, column);
}
