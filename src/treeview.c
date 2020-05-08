#include <gtk/gtk.h>
#include <jansson.h>
#include "otpclient.h"
#include "liststore-misc.h"
#include "gui-common.h"
#include "message-dialogs.h"


typedef struct _parsed_json_data {
    gchar **types;
    gchar **labels;
    gchar **issuers;
    GArray *periods;
} ParsedData;

static void     set_json_data      (json_t         *array,
                                    ParsedData     *pjd);

static void     add_data_to_model  (DatabaseData   *db_data,
                                    GtkListStore   *store);

static void     add_columns        (GtkTreeView    *tree_view);

static void     hide_all_otps_cb   (GtkTreeView    *tree_view,
                                    gpointer        user_data);

static gboolean clear_all_otps     (GtkTreeModel   *model,
                                    GtkTreePath    *path,
                                    GtkTreeIter    *iter,
                                    gpointer        user_data);

static void     free_pjd           (ParsedData     *pjd);

static void     get_sort_data      (gint *column_id, GtkSortType *sort_order);


void
create_treeview (AppData *app_data)
{
    g_signal_new ("hide-all-otps", G_TYPE_OBJECT, G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);

    app_data->tree_view = GTK_TREE_VIEW(gtk_builder_get_object (app_data->builder, "treeview_id"));

    GtkBindingSet *binding_set = gtk_binding_set_by_class (GTK_TREE_VIEW_GET_CLASS(app_data->tree_view));
    gtk_binding_entry_add_signal (binding_set, GDK_KEY_h, GDK_CONTROL_MASK, "hide-all-otps", 0);

    GtkListStore *list_store = gtk_list_store_new (NUM_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                                   G_TYPE_UINT, G_TYPE_UINT, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_INT);

    add_columns (app_data->tree_view);

    add_data_to_model (app_data->db_data, list_store);

    gtk_tree_view_set_model (app_data->tree_view, GTK_TREE_MODEL(list_store));

    gint column_id = -1;
    GtkSortType sort_order = -1;
    get_sort_data (&column_id, &sort_order);
    if (column_id >= 0 && sort_order >= 0) {
        gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE(list_store), column_id, sort_order);
    }

    // model has id 0 for type, 1 for label, 2 for issuer, etc while ui file has 0 label and 1 issuer. That's why the  "+1"
    gtk_tree_view_set_search_column (GTK_TREE_VIEW(app_data->tree_view), app_data->search_column + 1);

    // signal emitted when row is selected
    g_signal_connect (app_data->tree_view, "row-activated", G_CALLBACK(row_selected_cb), app_data);

    // signal emitted when CTRL+H is pressed
    g_signal_connect (app_data->tree_view, "hide-all-otps", G_CALLBACK(hide_all_otps_cb), app_data);

    g_object_unref (list_store);
}


void
update_model (AppData *app_data)
{
    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model (app_data->tree_view));

    gtk_list_store_clear (store);

    add_data_to_model (app_data->db_data, store);
}


void
delete_rows_cb (GtkTreeView        *tree_view,
                GtkTreePath        *path,
                GtkTreeViewColumn  *column    __attribute__((unused)),
                gpointer            user_data)
{
    AppData *app_data = (AppData *)user_data;

    g_return_if_fail (tree_view != NULL);

    GtkTreeModel *model = gtk_tree_view_get_model (tree_view);
    GtkListStore *list_store = GTK_LIST_STORE(model);

    GtkTreeIter  iter;
    gtk_tree_model_get_iter (model, &iter, path);

    gint db_item_position_to_delete;
    gtk_tree_model_get (model, &iter, COLUMN_POSITION_IN_DB, &db_item_position_to_delete, -1);

    json_array_remove (app_data->db_data->json_data, db_item_position_to_delete);
    gtk_list_store_remove (list_store, &iter);

    // json_array_remove shifts all items, so we have to take care of updating the real item's position in the database
    gint row_db_pos;
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    while (valid) {
        gtk_tree_model_get (model, &iter, COLUMN_POSITION_IN_DB, &row_db_pos, -1);
        if (row_db_pos > db_item_position_to_delete) {
            gint shifted_position = row_db_pos - 1;
            gtk_list_store_set (list_store, &iter, COLUMN_POSITION_IN_DB, shifted_position, -1);
        }
        valid = gtk_tree_model_iter_next(model, &iter);
    }

    GError *err = NULL;
    update_and_reload_db (app_data, app_data->db_data, FALSE, &err);
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
    AppData *app_data = (AppData *)user_data;
    GtkTreeModel *model = gtk_tree_view_get_model (tree_view);

    GtkTreeIter  iter;
    gtk_tree_model_get_iter (model, &iter, path);

    gchar *otp_type, *otp_value;
    gtk_tree_model_get (model, &iter, COLUMN_TYPE, &otp_type, -1);
    gtk_tree_model_get (model, &iter, COLUMN_OTP, &otp_value, -1);

    GDateTime *now = g_date_time_new_now_local ();
    GTimeSpan diff = g_date_time_difference (now, app_data->db_data->last_hotp_update);
    if (otp_value != NULL && g_utf8_strlen (otp_value, -1) > 3) {
        // OTP is already set, so we update the value only if it is an HOTP
        if (g_ascii_strcasecmp (otp_type, "HOTP") == 0) {
            if (diff >= G_USEC_PER_SEC * HOTP_RATE_LIMIT_IN_SEC) {
                set_otp (GTK_LIST_STORE (model), iter, app_data);
                g_free (otp_value);
                gtk_tree_model_get (model, &iter, COLUMN_OTP, &otp_value, -1);
            }
        }
    } else {
        // OTP is not already set, so we set it
        set_otp (GTK_LIST_STORE (model), iter, app_data);
        g_free (otp_value);
        gtk_tree_model_get (model, &iter, COLUMN_OTP, &otp_value, -1);
    }
    // and, in any case, we copy the otp to the clipboard and send a notification
    gtk_clipboard_set_text (app_data->clipboard, otp_value, -1);
    if (!app_data->disable_notifications) {
        g_application_send_notification (G_APPLICATION(gtk_window_get_application (GTK_WINDOW(app_data->main_window))), NOTIFICATION_ID, app_data->notification);
    }

    g_date_time_unref (now);
    g_free (otp_type);
    g_free (otp_value);
}


void
reset_column_sorting_cb (GSimpleAction *simple    __attribute__((unused)),
                         GVariant      *parameter __attribute__((unused)),
                         gpointer       user_data)
{
    AppData *app_data = (AppData *)user_data;
    GError *err = NULL;
    gchar *err_msg;
    GKeyFile *kf = g_key_file_new ();
    gchar *cfg_file_path;
#ifndef USE_FLATPAK_APP_FOLDER
    cfg_file_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
#else
    cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#endif
    if (g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
        if (!g_key_file_load_from_file (kf, cfg_file_path, G_KEY_FILE_NONE, &err)) {
            g_printerr ("%s\n", err->message);
        } else {
            if (g_key_file_has_key (kf, "config", "column_id", NULL)) {
                if (!g_key_file_remove_key (kf, "config", "column_id", &err) && err != NULL) {
                    err_msg = g_strconcat ("Couldn't reset the column id: ", err->message, NULL);
                    show_message_dialog (app_data->main_window, err_msg, GTK_MESSAGE_ERROR);
                    g_free (err_msg);
                    g_clear_error (&err);
                }
            }
            if (g_key_file_has_key (kf, "config", "sort_order", NULL)) {
                if (!g_key_file_remove_key (kf, "config", "sort_order", &err) && err != NULL) {
                    err_msg = g_strconcat ("Couldn't reset the sorting order: ", err->message, NULL);
                    show_message_dialog (app_data->main_window, err_msg, GTK_MESSAGE_ERROR);
                    g_free (err_msg);
                    g_clear_error (&err);
                }
            }
            if (!g_key_file_save_to_file (kf, cfg_file_path, &err) && err != NULL) {
                err_msg = g_strconcat ("Couldn't save the configuration file: ", err->message, NULL);
                show_message_dialog (app_data->main_window, err_msg, GTK_MESSAGE_ERROR);
                g_free (err_msg);
                g_clear_error (&err);
            }
        }
    }
    g_key_file_free (kf);
    g_free (cfg_file_path);

    // set default sorting value
    gtk_tree_sortable_set_sort_column_id (GTK_TREE_SORTABLE(GTK_LIST_STORE(gtk_tree_view_get_model (app_data->tree_view))), -2, 0);

    show_message_dialog (app_data->main_window, "Sorting order has been correctly reset.\nPlease close and open the program again to apply the changes.", GTK_MESSAGE_INFO);
}


static void
hide_all_otps_cb (GtkTreeView *tree_view,
                  gpointer     user_data)
{
    gtk_tree_model_foreach (GTK_TREE_MODEL(gtk_tree_view_get_model (tree_view)), clear_all_otps, user_data);
}


static gboolean
clear_all_otps (GtkTreeModel *model,
                GtkTreePath  *path      __attribute__((unused)),
                GtkTreeIter  *iter,
                gpointer      user_data __attribute__((unused)))
{
    gchar *otp;
    gtk_tree_model_get (model, iter, COLUMN_OTP, &otp, -1);

    if (otp != NULL && g_utf8_strlen (otp, -1) > 4) {
        gtk_list_store_set (GTK_LIST_STORE(model), iter, COLUMN_OTP, "", COLUMN_VALIDITY, 0, COLUMN_UPDATED, FALSE, COLUMN_LESS_THAN_A_MINUTE, FALSE, -1);
    }

    g_free (otp);

    // do not stop walking the store, check next row
    return FALSE;
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
                            COLUMN_PERIOD, g_array_index (pjd->periods, gint, i),
                            COLUMN_UPDATED, FALSE,
                            COLUMN_LESS_THAN_A_MINUTE, FALSE,
                            COLUMN_POSITION_IN_DB, i,
                            -1);
        i++;
    }
    free_pjd (pjd);
}


static void
add_columns (GtkTreeView *tree_view)
{
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new ();
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes ("Type", renderer, "text", COLUMN_TYPE, NULL);
    gtk_tree_view_append_column (tree_view, column);

    renderer = gtk_cell_renderer_text_new ();
    column = gtk_tree_view_column_new_with_attributes ("Account", renderer, "text", COLUMN_ACC_LABEL, NULL);
    gtk_tree_view_column_set_sizing (GTK_TREE_VIEW_COLUMN(column), GTK_TREE_VIEW_COLUMN_AUTOSIZE);
    gtk_tree_view_column_set_clickable (GTK_TREE_VIEW_COLUMN(column), TRUE);
    gtk_tree_view_column_set_sort_column_id (GTK_TREE_VIEW_COLUMN(column), 1); // 1 is the account column
    gtk_tree_view_append_column (tree_view, column);

    renderer = gtk_cell_renderer_text_new ();
    column = gtk_tree_view_column_new_with_attributes ("Issuer", renderer, "text", COLUMN_ACC_ISSUER, NULL);
    gtk_tree_view_column_set_sizing (GTK_TREE_VIEW_COLUMN(column), GTK_TREE_VIEW_COLUMN_AUTOSIZE);
    gtk_tree_view_column_set_clickable (GTK_TREE_VIEW_COLUMN(column), TRUE);
    gtk_tree_view_column_set_sort_column_id (GTK_TREE_VIEW_COLUMN(column), 2); // 2 is the issuer column
    gtk_tree_view_append_column (tree_view, column);

    renderer = gtk_cell_renderer_text_new ();
    column = gtk_tree_view_column_new_with_attributes ("OTP Value", renderer, "text", COLUMN_OTP, NULL);
    gtk_tree_view_column_set_sizing (GTK_TREE_VIEW_COLUMN(column), GTK_TREE_VIEW_COLUMN_AUTOSIZE);
    gtk_tree_view_append_column (tree_view, column);

    renderer = gtk_cell_renderer_text_new ();
    column = gtk_tree_view_column_new_with_attributes ("Validity", renderer, "text", COLUMN_VALIDITY, NULL);
    gtk_tree_view_column_set_sizing (GTK_TREE_VIEW_COLUMN(column), GTK_TREE_VIEW_COLUMN_AUTOSIZE);
    gtk_tree_view_append_column (tree_view, column);

    renderer = gtk_cell_renderer_text_new ();
    column = gtk_tree_view_column_new_with_attributes ("Period", renderer, "text", COLUMN_PERIOD, NULL);
    gtk_tree_view_column_set_visible (column, FALSE);
    gtk_tree_view_append_column (tree_view, column);

    renderer = gtk_cell_renderer_text_new ();
    column = gtk_tree_view_column_new_with_attributes ("Updated", renderer, "text", COLUMN_UPDATED, NULL);
    gtk_tree_view_column_set_visible (column, FALSE);
    gtk_tree_view_append_column (tree_view, column);

    renderer = gtk_cell_renderer_text_new ();
    column = gtk_tree_view_column_new_with_attributes ("Less Than a Minute", renderer, "text", COLUMN_LESS_THAN_A_MINUTE, NULL);
    gtk_tree_view_column_set_visible (column, FALSE);
    gtk_tree_view_append_column (tree_view, column);

    renderer = gtk_cell_renderer_text_new ();
    column = gtk_tree_view_column_new_with_attributes ("Position in Database", renderer, "text", COLUMN_POSITION_IN_DB, NULL);
    gtk_tree_view_column_set_visible (column, FALSE);
    gtk_tree_view_append_column (tree_view, column);
}


static void
free_pjd (ParsedData *pjd)
{
    g_strfreev (pjd->types);
    g_strfreev (pjd->labels);
    g_strfreev (pjd->issuers);
    g_array_free (pjd->periods, TRUE);
    g_free (pjd);
}


static void
get_sort_data (gint         *column_id,
               GtkSortType  *sort_order)
{
    GError *err = NULL;
    GKeyFile *kf = g_key_file_new ();
    gchar *cfg_file_path;
#ifndef USE_FLATPAK_APP_FOLDER
    cfg_file_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
#else
    cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#endif
    if (g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
        if (!g_key_file_load_from_file (kf, cfg_file_path, G_KEY_FILE_NONE, &err)) {
            g_printerr ("%s\n", err->message);
        } else {
            *column_id = g_key_file_get_integer (kf, "config", "column_id", &err);
            if (err != NULL && (err->code == G_KEY_FILE_ERROR_KEY_NOT_FOUND || err->code == G_KEY_FILE_ERROR_INVALID_VALUE)) {
                *column_id = -1;
                *sort_order = -1;
            } else {
                *sort_order = g_key_file_get_integer (kf, "config", "sort_order", &err);
                if (err != NULL && (err->code == G_KEY_FILE_ERROR_KEY_NOT_FOUND || err->code == G_KEY_FILE_ERROR_INVALID_VALUE)) {
                    *column_id = -1;
                    *sort_order = -1;
                }
            }
        }
    }
    g_key_file_free (kf);
    g_free (cfg_file_path);
}
