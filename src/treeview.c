#include <gtk/gtk.h>
#include "otpclient.h"
#include "timer.h"
#include <cotp.h>
#include <json-glib/json-types.h>
#include "liststore-misc.h"


typedef struct _parsed_json_data {
    gchar **types;
    gchar **labels;
    gchar **issuers;
} ParsedData;

static void set_json_data (JsonNode *root_json_node, ParsedData *pjd);

static void add_data_to_model (DatabaseData *db_data, GtkListStore *store);

static GtkTreeModel *create_model (DatabaseData *db_data);

static void add_columns (GtkTreeView *treeview, DatabaseData *db_data);

static void free_parsed_json_data (ParsedData *pjd);


GtkListStore *
create_treeview (GtkWidget      *main_win,
                 DatabaseData   *db_data)
{
    GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_add (GTK_CONTAINER (main_win), vbox);

    GtkWidget *timer_label = gtk_label_new (NULL);
    gtk_box_pack_start (GTK_BOX (vbox), timer_label, FALSE, FALSE, 5);

    GtkWidget *sw = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (sw), GTK_SHADOW_ETCHED_IN);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start (GTK_BOX (vbox), sw, TRUE, TRUE, 0);

    GtkTreeModel *model = create_model (db_data);

    GtkWidget *treeview = gtk_tree_view_new_with_model (model);
    gtk_tree_view_set_search_column (GTK_TREE_VIEW (treeview), COLUMN_ACC_LABEL);

    g_object_unref (model);

    g_object_set_data (G_OBJECT (timer_label), "lstore", GTK_LIST_STORE (model));
    g_object_set_data (G_OBJECT (timer_label), "db_data", db_data);
    g_timeout_add_seconds (1, label_update, timer_label);

    gtk_container_add (GTK_CONTAINER (sw), treeview);

    add_columns (GTK_TREE_VIEW (treeview), db_data);

    return GTK_LIST_STORE (model);
}


void
update_model (DatabaseData *db_data,
              GtkListStore *store)
{
    gtk_list_store_clear (store);

    add_data_to_model (db_data, store);
}


static void
set_json_data (JsonNode     *root_json_node,
               ParsedData   *pjd)
{
    JsonArray *ja = json_node_get_array (root_json_node);
    guint ja_len = json_array_get_length (ja);
    JsonObject *jo;
    for (guint i = 0; i < ja_len; i++) {
        jo = json_array_get_object_element (ja, i);
        pjd->types[i] = g_strdup (json_object_get_string_member (jo, "otp"));
        pjd->labels[i] = g_strdup (json_object_get_string_member (jo, "label"));
        pjd->issuers[i] = g_strdup (json_object_get_string_member (jo, "issuer"));
    }
    pjd->types[ja_len] = NULL;
    pjd->labels[ja_len] = NULL;
    pjd->issuers[ja_len] = NULL;
}


static void
add_data_to_model (DatabaseData *db_data,
                   GtkListStore *store)
{
    GtkTreeIter iter;
    ParsedData *pjd = g_new0 (ParsedData, 1);

    set_json_data (db_data->json_data, pjd);

    gint i = 0;
    while (pjd->types[i] != NULL) {
        gtk_list_store_append (store, &iter);
        gtk_list_store_set (store, &iter,
                            COLUMN_BOOLEAN, FALSE,
                            COLUMN_TYPE, pjd->types[i],
                            COLUMN_ACC_LABEL, pjd->labels[i],
                            COLUMN_ACC_ISSUER, pjd->issuers[i],
                            COLUMN_OTP, "",
                            -1);
        i++;
    }
    free_parsed_json_data (pjd);
}


static GtkTreeModel *
create_model (DatabaseData *db_data)
{
    GtkListStore *store = gtk_list_store_new (NUM_COLUMNS, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);

    if (db_data->json_data != NULL) {
        add_data_to_model (db_data, store);
    }

    return GTK_TREE_MODEL (store);
}


static void
fixed_toggled (GtkCellRendererToggle    *cell __attribute__((__unused__)),
               gchar                    *path_str,
               gpointer                  data)
{
    GtkTreeModel *model = (GtkTreeModel *)data;
    GtkTreeIter  iter;
    GtkTreePath *path = gtk_tree_path_new_from_string (path_str);
    gboolean fixed;
    gchar *acc_label;

    gtk_tree_model_get_iter (model, &iter, path);
    gtk_tree_model_get (model, &iter, COLUMN_BOOLEAN, &fixed, COLUMN_ACC_LABEL, &acc_label, -1);

    if (fixed) {
        gtk_list_store_set (GTK_LIST_STORE (model), &iter, COLUMN_OTP, "", -1);
    } else {
        // TODO implement rate-limiting for HOTP (like 1 every 5s)
        DatabaseData *db_data = g_object_get_data (G_OBJECT (model), "data");
        set_otp (GTK_LIST_STORE (model), iter, db_data);
    }
    fixed ^= 1;
    gtk_list_store_set (GTK_LIST_STORE (model), &iter, COLUMN_BOOLEAN, fixed, -1);

    g_free (acc_label);

    gtk_tree_path_free (path);
}


static void
add_columns (GtkTreeView    *treeview,
             DatabaseData   *db_data)
{
    GtkCellRenderer *renderer;
    GtkTreeViewColumn *column;
    GtkTreeModel *model = gtk_tree_view_get_model (treeview);
    g_object_set_data (G_OBJECT (model), "data", db_data);

    renderer = gtk_cell_renderer_toggle_new ();
    g_signal_connect (renderer, "toggled", G_CALLBACK (fixed_toggled), model);

    column = gtk_tree_view_column_new_with_attributes ("Show", renderer, "active", COLUMN_BOOLEAN, NULL);
    gtk_tree_view_column_set_sizing (GTK_TREE_VIEW_COLUMN (column), GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width (GTK_TREE_VIEW_COLUMN (column), 50);
    gtk_tree_view_append_column (treeview, column);

    renderer = gtk_cell_renderer_text_new ();
    column = gtk_tree_view_column_new_with_attributes ("Type", renderer, "text", COLUMN_ACC_LABEL, NULL);
    gtk_tree_view_column_set_sort_column_id (column, COLUMN_TYPE);
    gtk_tree_view_append_column (treeview, column);

    renderer = gtk_cell_renderer_text_new ();
    column = gtk_tree_view_column_new_with_attributes ("Label", renderer, "text", COLUMN_ACC_LABEL, NULL);
    gtk_tree_view_column_set_sort_column_id (column, COLUMN_ACC_LABEL);
    gtk_tree_view_append_column (treeview, column);

    renderer = gtk_cell_renderer_text_new ();
    column = gtk_tree_view_column_new_with_attributes ("Issuer", renderer, "text", COLUMN_ACC_ISSUER, NULL);
    gtk_tree_view_column_set_sort_column_id (column, COLUMN_ACC_LABEL);
    gtk_tree_view_append_column (treeview, column);

    renderer = gtk_cell_renderer_text_new ();
    column = gtk_tree_view_column_new_with_attributes ("OTP Value", renderer, "text", COLUMN_OTP, NULL);
    gtk_tree_view_append_column (treeview, column);
}


static void
free_parsed_json_data (ParsedData *pjd)
{
    g_strfreev (pjd->types);
    g_strfreev (pjd->labels);
    g_strfreev (pjd->issuers);
    g_free (pjd);
}