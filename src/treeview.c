#include <gtk/gtk.h>
#include "otpclient.h"
#include "timer.h"
#include <cotp.h>
#include "liststore-misc.h"


static gchar **get_account_names (const gchar *dec_kf);

static GtkTreeModel *create_model (gchar **account_names);

static void add_columns (GtkTreeView *treeview, UpdateData *kf_data);


GtkListStore *
create_treeview (GtkWidget *main_win, UpdateData *kf_update_data)
{
    GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_add (GTK_CONTAINER (main_win), vbox);

    GtkWidget *timer_label = gtk_label_new (NULL);
    gtk_box_pack_start (GTK_BOX (vbox), timer_label, FALSE, FALSE, 5);

    GtkWidget *sw = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (sw), GTK_SHADOW_ETCHED_IN);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start (GTK_BOX (vbox), sw, TRUE, TRUE, 0);

    gchar **account_names = get_account_names (kf_update_data->in_memory_kf);
    GtkTreeModel *model = create_model (account_names);
    g_strfreev (account_names);

    GtkWidget *treeview = gtk_tree_view_new_with_model (model);
    gtk_tree_view_set_search_column (GTK_TREE_VIEW (treeview), COLUMN_ACNM);

    g_object_unref (model);

    g_object_set_data (G_OBJECT (timer_label), "lstore", GTK_LIST_STORE (model));
    g_object_set_data (G_OBJECT (timer_label), "kf_data", kf_update_data);
    g_timeout_add_seconds (1, label_update, timer_label);

    gtk_container_add (GTK_CONTAINER (sw), treeview);

    add_columns (GTK_TREE_VIEW (treeview), kf_update_data);

    return GTK_LIST_STORE (model);
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
    GtkTreeIter iter;
    GtkListStore *store = gtk_list_store_new (NUM_COLUMNS, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING);

    gint i = 0;
    while (account_names[i] != NULL) {
        gtk_list_store_append (store, &iter);
        gtk_list_store_set (store, &iter, COLUMN_BOOLEAN, FALSE, COLUMN_ACNM, account_names[i], COLUMN_OTP, "", -1);
        i++;
    }

    return GTK_TREE_MODEL (store);
}


static void
fixed_toggled (GtkCellRendererToggle *cell, gchar *path_str, gpointer data)
{
    GtkTreeModel *model = (GtkTreeModel *)data;
    GtkTreeIter  iter;
    GtkTreePath *path = gtk_tree_path_new_from_string (path_str);
    gboolean fixed;
    gchar *account_name;

    gtk_tree_model_get_iter (model, &iter, path);
    gtk_tree_model_get (model, &iter, COLUMN_BOOLEAN, &fixed, COLUMN_ACNM, &account_name, -1);

    if (fixed) {
        gtk_list_store_set (GTK_LIST_STORE (model), &iter, COLUMN_OTP, "", -1);
    } else {
        UpdateData *kf_data = g_object_get_data (G_OBJECT (model), "data");
        set_otp (GTK_LIST_STORE (model), iter, account_name, kf_data);
    }
    fixed ^= 1;
    gtk_list_store_set (GTK_LIST_STORE (model), &iter, COLUMN_BOOLEAN, fixed, -1);

    g_free (account_name);

    gtk_tree_path_free (path);
}


static void
add_columns (GtkTreeView *treeview, UpdateData *kf_data)
{
    GtkCellRenderer *renderer;
    GtkTreeViewColumn *column;
    GtkTreeModel *model = gtk_tree_view_get_model (treeview);
    g_object_set_data (G_OBJECT (model), "data", kf_data);

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


void
update_model (UpdateData *kf_data, GtkListStore *store)
{
    GtkTreeIter iter;
    gtk_list_store_clear (store);
    gchar **account_names = get_account_names (kf_data->in_memory_kf);
    gint i = 0;
    while (account_names[i] != NULL) {
        gtk_list_store_append (store, &iter);
        gtk_list_store_set (store, &iter, COLUMN_BOOLEAN, FALSE, COLUMN_ACNM, account_names[i],
                            COLUMN_OTP, "", -1);
        i++;
    }
    g_strfreev (account_names);
}