#include <gtk/gtk.h>
#include <cotp.h>
#include "otpclient.h"
#include "timer.h"
#include "liststore-misc.h"
#include "common.h"


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

    // signal sent when selected row is double clicked
    //g_signal_connect (treeview, "row-activated", G_CALLBACK (row_selected_cb), NULL);

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


void
remove_selected_entries (DatabaseData *db_data,
                         GtkListStore *list_store)
{
    GtkTreeIter iter;
    gboolean valid, is_active;
    GError *err = NULL;
    JsonArray *ja = json_node_get_array (db_data->json_data);

    g_return_if_fail (list_store != NULL);

    valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (list_store), &iter);

    while (valid) {
        gtk_tree_model_get (GTK_TREE_MODEL (list_store), &iter, COLUMN_BOOLEAN, &is_active, -1);
        if (is_active) {
            guint row_number = get_row_number_from_iter (list_store, iter);
            json_array_remove_element (ja, row_number);
            gtk_list_store_remove (list_store, &iter);
            valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (list_store), &iter);
        } else {
            valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (list_store), &iter);
        }
    }
    update_and_reload_db (db_data, list_store, FALSE, &err);
    if (err != NULL) {
        g_printerr ("%s\n", err->message);
    }
}


static void
set_json_data (JsonNode     *root_json_node,
               ParsedData   *pjd)
{
    JsonArray *ja = json_node_get_array (root_json_node);
    guint ja_len = json_array_get_length (ja);
    JsonObject *jo;
    pjd->types = (gchar **) g_malloc0 ((ja_len + 1)  * sizeof (gchar *));
    pjd->labels = (gchar **) g_malloc0 ((ja_len + 1) * sizeof (gchar *));
    pjd->issuers = (gchar **) g_malloc0 ((ja_len + 1) * sizeof (gchar *));
    for (guint i = 0; i < ja_len; i++) {
        jo = json_array_get_object_element (ja, i);
        pjd->types[i] = g_strdup (json_object_get_string_member (jo, "type"));
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
    GtkTreeModel *model = (GtkTreeModel *) data;
    GtkTreeIter  iter;
    GtkTreePath *path = gtk_tree_path_new_from_string (path_str);

    gtk_tree_model_get_iter (model, &iter, path);

    gboolean fixed;
    gtk_tree_model_get (model, &iter, COLUMN_BOOLEAN, &fixed, -1);

    gchar *otp_type;
    gtk_tree_model_get (model, &iter, COLUMN_TYPE, &otp_type, -1);

    if (fixed) {
        gtk_list_store_set (GTK_LIST_STORE (model), &iter, COLUMN_OTP, "", -1);
    } else {
        DatabaseData *db_data = g_object_get_data (G_OBJECT (model), "data");
        GDateTime *now = g_date_time_new_now_local ();
        GTimeSpan diff = g_date_time_difference (now, db_data->last_hotp_update);
        if (g_strcmp0 (otp_type, "HOTP") == 0 && diff < G_USEC_PER_SEC * HOTP_RATE_LIMIT_IN_SEC) {
            gtk_list_store_set (GTK_LIST_STORE (model), &iter, COLUMN_OTP, db_data->last_hotp, -1);
        } else {
            set_otp (GTK_LIST_STORE (model), iter, db_data);
        }
        g_date_time_unref (now);
    }
    fixed ^= 1;
    gtk_list_store_set (GTK_LIST_STORE (model), &iter, COLUMN_BOOLEAN, fixed, -1);

    g_free (otp_type);
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
    column = gtk_tree_view_column_new_with_attributes ("Type", renderer, "text", COLUMN_TYPE, NULL);
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