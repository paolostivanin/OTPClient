#include <gtk/gtk.h>


enum {
    COLUMN_BOOLEAN,
    COLUMN_ACNM,
    COLUMN_OTP,
    NUM_COLUMNS
};


static GtkTreeModel *
create_model (void)
{
    GtkListStore *store;
    GtkTreeIter iter;

    /* create list store */
    store = gtk_list_store_new (NUM_COLUMNS,
                                G_TYPE_BOOLEAN,
                                G_TYPE_STRING,
                                G_TYPE_STRING);

    /* add data to the list store */
    gtk_list_store_append (store, &iter);
    gtk_list_store_set (store, &iter,
                        COLUMN_BOOLEAN, FALSE,
                        COLUMN_ACNM, "Google",
                        COLUMN_OTP, "",
                        -1);
    gtk_list_store_append (store, &iter);
    gtk_list_store_set (store, &iter,
                        COLUMN_BOOLEAN, FALSE,
                        COLUMN_ACNM, "Amazon",
                        COLUMN_OTP, "",
                        -1);  

    return GTK_TREE_MODEL (store);
}


static void
fixed_toggled (GtkCellRendererToggle *cell __attribute__((__unused__)),
               gchar                 *path_str,
               gpointer               data)
{
    GtkTreeModel *model = (GtkTreeModel *)data;
    GtkTreeIter  iter;
    GtkTreePath *path = gtk_tree_path_new_from_string (path_str);
    gboolean fixed;

    /* get toggled iter */
    gtk_tree_model_get_iter (model, &iter, path);
    gtk_tree_model_get (model, &iter, COLUMN_BOOLEAN, &fixed, -1);

    /* do something with the value */
    fixed ^= 1;

    /* set new value */
    gtk_list_store_set (GTK_LIST_STORE (model), &iter, COLUMN_BOOLEAN, fixed, -1);

    /* clean up */
    gtk_tree_path_free (path);
}


static void
add_columns (GtkTreeView *treeview)
{
    GtkCellRenderer *renderer;
    GtkTreeViewColumn *column;
    GtkTreeModel *model = gtk_tree_view_get_model (treeview);

    /* column for fixed toggles */
    renderer = gtk_cell_renderer_toggle_new ();
    g_signal_connect (renderer, "toggled",
                    G_CALLBACK (fixed_toggled), model);

    column = gtk_tree_view_column_new_with_attributes ( "Show",
                                                        renderer,
                                                        "active", COLUMN_BOOLEAN,
                                                        NULL);

    /* set this column to a fixed sizing (of 50 pixels) */
    gtk_tree_view_column_set_sizing (GTK_TREE_VIEW_COLUMN (column),
                                   GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width (GTK_TREE_VIEW_COLUMN (column), 50);
    gtk_tree_view_append_column (treeview, column);

    /* column for severities */
    renderer = gtk_cell_renderer_text_new ();
    column = gtk_tree_view_column_new_with_attributes ( "Account Name",
                                                        renderer,
                                                        "text",
                                                        COLUMN_ACNM,
                                                        NULL);
    gtk_tree_view_column_set_sort_column_id (column, COLUMN_ACNM);
    gtk_tree_view_append_column (treeview, column);

    /* column for description */
    renderer = gtk_cell_renderer_text_new ();
    column = gtk_tree_view_column_new_with_attributes ( "OTP Value",
                                                        renderer,
                                                        "text",
                                                        COLUMN_OTP,
                                                        NULL);
    gtk_tree_view_column_set_sort_column_id (column, COLUMN_OTP);
    gtk_tree_view_append_column (treeview, column);
}


int
main (  int argc,
        char *argv[])
{
    GtkWidget *window;
    GtkWidget *vbox;
    GtkWidget *label;
    GtkWidget *sw;
    GtkTreeModel *model;
    GtkWidget *treeview;

    gtk_init (&argc, &argv);

    window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
    gtk_window_set_screen (GTK_WINDOW (window),  gdk_screen_get_default ());

    gtk_window_set_title(GTK_WINDOW(window), "OTP Client");
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    gtk_container_set_border_width (GTK_CONTAINER (window), 8);

    gtk_widget_set_size_request (GTK_WIDGET (window), 350, 250);
    
    g_signal_connect (window, "destroy", G_CALLBACK (gtk_main_quit), window);

    vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_add (GTK_CONTAINER (window), vbox);

    label = gtk_label_new("GTK+ OTP Client");
    gtk_box_pack_start (GTK_BOX (vbox), label, FALSE, FALSE, 0);

    sw = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (sw), GTK_SHADOW_ETCHED_IN);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start (GTK_BOX (vbox), sw, TRUE, TRUE, 0);

    /* create tree model */
    model = create_model ();

    /* create tree view */
    treeview = gtk_tree_view_new_with_model (model);
    gtk_tree_view_set_search_column (GTK_TREE_VIEW (treeview), COLUMN_ACNM);

    g_object_unref (model);

    gtk_container_add (GTK_CONTAINER (sw), treeview);

    /* add columns to the tree view */
    add_columns (GTK_TREE_VIEW (treeview));

    /* finish & show */
    gtk_window_set_default_size (GTK_WINDOW (window), 280, 250); 
    gtk_widget_show_all (window);
    
    gtk_main ();

    return 0;
}
