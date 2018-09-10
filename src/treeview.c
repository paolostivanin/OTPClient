#include <gtk/gtk.h>
#include <cotp.h>
#include <jansson.h>
#include "otpclient.h"
#include "liststore-misc.h"
#include "common.h"
#include "app.h"
#include "message-dialogs.h"


typedef struct _parsed_json_data {
    gchar **types;
    gchar **labels;
    gchar **issuers;
    GArray *periods;
} ParsedData;


static gboolean      label_update           (gpointer data);

static void          set_json_data          (json_t *array, ParsedData *pjd);

static void          add_data_to_model      (DatabaseData *db_data, GtkListStore *store);

static GtkTreeModel *create_model           (DatabaseData *db_data);

static void          add_columns            (GtkTreeView *treeview);

static void          row_selected_cb        (GtkTreeView *tree_view, GtkTreePath *path, GtkTreeViewColumn *column, gpointer user_data);

static void          free_parsed_json_data  (ParsedData *pjd);


void
create_treeview (AppData *app_data)
{
    // Because we rely on the order in which data has been added to the model when deleting a row, columns must NOT be clickable/reordable

    GtkBuilder *builder = get_builder_from_partial_path (UI_PARTIAL_PATH);

    app_data->tree_view = GTK_TREE_VIEW(gtk_builder_get_object (builder, "treeview_id"));
    
    GtkListStore *list_store = GTK_LIST_STORE(gtk_builder_get_object (builder, "liststore_model_id"));

    add_columns (app_data->tree_view);

    add_data_to_model (app_data->db_data, list_store);
    
    // model has id 0 for type, 1 for label, 2 for issuer, etc while ui file has 0 label and 1 issuer. That's why the  "+1"
    gtk_tree_view_set_search_column (GTK_TREE_VIEW(app_data->tree_view), app_data->search_column + 1);

    // signal sent when row is selected
    g_signal_connect (app_data->tree_view, "row-activated", G_CALLBACK(row_selected_cb), app_data->clipboard);

    g_object_unref (builder);
}


void
update_model (DatabaseData *db_data,
              GtkTreeView  *tree_view)
{
    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model (tree_view));

    gtk_list_store_clear (store);

    add_data_to_model (db_data, store);
}


void
delete_rows_cb (GtkTreeView        *tree_view,
                GtkTreePath        *path,
                GtkTreeViewColumn  *column    __attribute__((unused)),
                gpointer            user_data)
{
    AppData *app_data = (AppData *)user_data;
    DatabaseData *db_data = app_data->db_data;

    g_return_if_fail (tree_view != NULL);
  
    GtkTreeModel *model = gtk_tree_view_get_model (tree_view);
    GtkListStore *list_store = GTK_LIST_STORE(model);

    GtkTreeIter  iter;
    gtk_tree_model_get_iter (model, &iter, path);

    guint row_number = get_row_number_from_iter (list_store, iter);
    json_array_remove (db_data->json_data, row_number);
    gtk_list_store_remove (list_store, &iter);
    
    GError *err = NULL;
    update_and_reload_db (db_data, list_store, FALSE, &err);
    if (err != NULL) {
        gchar *msg = g_strconcat ("The database update <b>FAILED</b>. The error message is:\n", err->message, NULL);
        show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
        g_free (msg);
    }
}


void
row_selected_cb (GtkTreeView        *tree_view,
                 GtkTreePath        *path,
                 GtkTreeViewColumn  *column    __attribute__((unused)),
                 gpointer            user_data)
{
    GtkClipboard *clipboard = (GtkClipboard *) user_data;
    GtkTreeModel *model = gtk_tree_view_get_model (tree_view);

    GtkTreeIter  iter;
    gtk_tree_model_get_iter (model, &iter, path);

    gchar *otp_value;
    gtk_tree_model_get (model, &iter, COLUMN_OTP, &otp_value, -1);

    gtk_clipboard_set_text (clipboard, otp_value, -1);

    g_free (otp_value);

/*     GtkTreeModel *model = (GtkTreeModel *) data;
    GtkTreeIter  iter;
    GtkTreePath *path = gtk_tree_path_new_from_string (path_str);

    gtk_tree_model_get_iter (model, &iter, path);

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
            gtk_clipboard_set_text (clipboard, db_data->last_hotp, -1);
        } else {
            set_otp (GTK_LIST_STORE (model), iter, db_data);
            gchar *otp_value;
            gtk_tree_model_get (model, &iter, COLUMN_OTP, &otp_value, -1);
            gtk_clipboard_set_text (clipboard, otp_value, -1);
            g_free (otp_value);
        }
        g_date_time_unref (now);
    }

    g_free (otp_type);
    gtk_tree_path_free (path); */
}


static void
set_json_data (json_t     *array,
               ParsedData *pjd)
{
    gsize array_len = json_array_size (array);
    pjd->types = (gchar **) g_malloc0 ((array_len + 1)  * sizeof (gchar *));
    pjd->labels = (gchar **) g_malloc0 ((array_len + 1) * sizeof (gchar *));
    pjd->issuers = (gchar **) g_malloc0 ((array_len + 1) * sizeof (gchar *));
    pjd->periods = g_array_new (FALSE, FALSE, sizeof(gint));
    for (guint i = 0; i < array_len; i++) {
        json_t *obj = json_array_get (array, i);
        pjd->types[i] = g_strdup (json_string_value (json_object_get (obj, "type")));
        pjd->labels[i] = g_strdup (json_string_value (json_object_get (obj, "label")));
        pjd->issuers[i] = g_strdup (json_string_value (json_object_get (obj, "issuer")));
        json_int_t period = json_integer_value (json_object_get (obj, "period"));
        g_array_append_val (pjd->periods, period);
    }
    pjd->types[array_len] = NULL;
    pjd->labels[array_len] = NULL;
    pjd->issuers[array_len] = NULL;
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
                            COLUMN_TYPE, pjd->types[i],
                            COLUMN_ACC_LABEL, pjd->labels[i],
                            COLUMN_ACC_ISSUER, pjd->issuers[i],
                            COLUMN_OTP, "",
                            COLUMN_VALIDITY, "",
                            COLUMN_PERIOD, g_array_index (pjd->periods, gint, i),
                            -1);
        i++;
    }
    free_parsed_json_data (pjd);
}


static void
add_columns (GtkTreeView *tree_view)
{
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new ();
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes ("Type", renderer, "text", COLUMN_TYPE, NULL);
    gtk_tree_view_append_column (tree_view, column);

    renderer = gtk_cell_renderer_text_new ();
    column = gtk_tree_view_column_new_with_attributes ("Label", renderer, "text", COLUMN_ACC_LABEL, NULL);
    gtk_tree_view_column_set_sizing (GTK_TREE_VIEW_COLUMN (column), GTK_TREE_VIEW_COLUMN_AUTOSIZE);
    gtk_tree_view_append_column (tree_view, column);

    renderer = gtk_cell_renderer_text_new ();
    column = gtk_tree_view_column_new_with_attributes ("Issuer", renderer, "text", COLUMN_ACC_ISSUER, NULL);
    gtk_tree_view_column_set_sizing (GTK_TREE_VIEW_COLUMN (column), GTK_TREE_VIEW_COLUMN_AUTOSIZE);
    gtk_tree_view_append_column (tree_view, column);

    renderer = gtk_cell_renderer_text_new ();
    column = gtk_tree_view_column_new_with_attributes ("OTP Value", renderer, "text", COLUMN_OTP, NULL);
    gtk_tree_view_column_set_sizing (GTK_TREE_VIEW_COLUMN (column), GTK_TREE_VIEW_COLUMN_AUTOSIZE);
    gtk_tree_view_append_column (tree_view, column);

    renderer = gtk_cell_renderer_text_new ();
    column = gtk_tree_view_column_new_with_attributes ("Validity", renderer, "text", COLUMN_VALIDITY, NULL);
    gtk_tree_view_column_set_sizing (GTK_TREE_VIEW_COLUMN (column), GTK_TREE_VIEW_COLUMN_AUTOSIZE);
    gtk_tree_view_append_column (tree_view, column);

    renderer = gtk_cell_renderer_text_new ();
    column = gtk_tree_view_column_new_with_attributes ("Period", renderer, "text", COLUMN_PERIOD, NULL);
    gtk_tree_view_column_set_visible (column, FALSE);
    gtk_tree_view_append_column (tree_view, column);
}


static void
free_parsed_json_data (ParsedData *pjd)
{
    g_strfreev (pjd->types);
    g_strfreev (pjd->labels);
    g_strfreev (pjd->issuers);
    g_array_free (pjd->periods, TRUE);
    g_free (pjd);
}
