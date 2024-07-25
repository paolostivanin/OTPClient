#include <gtk/gtk.h>
#include <jansson.h>
#include "../common/macros.h"
#include "otpclient.h"
#include "liststore-misc.h"
#include "message-dialogs.h"
#include "edit-row-cb.h"
#include "show-qr-cb.h"


typedef struct parsed_json_data_t {
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

static void     delete_row         (AppData        *app_data);

static void     hide_all_otps_cb   (GtkTreeView    *tree_view,
                                    gpointer        user_data);

static gboolean clear_all_otps     (GtkTreeModel   *model,
                                    GtkTreePath    *path,
                                    GtkTreeIter    *iter,
                                    gpointer        user_data);

static void     free_pjd           (ParsedData     *pjd);

static gboolean on_treeview_button_press_event (GtkWidget *treeview, GdkEventButton *event, gpointer user_data);


void
create_treeview (AppData *app_data)
{
    app_data->tree_view = GTK_TREE_VIEW(gtk_builder_get_object (app_data->builder, "treeview_id"));

    GtkListStore *list_store = gtk_list_store_new (NUM_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                                   G_TYPE_UINT, G_TYPE_UINT, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_INT);

    add_columns (app_data->tree_view);

    add_data_to_model (app_data->db_data, list_store);

    gtk_tree_view_set_model (app_data->tree_view, GTK_TREE_MODEL(list_store));

    // model has id 0 for type, 1 for label, 2 for issuer, etc while ui file has 0 label and 1 issuer. That's why the  "+1"
    gtk_tree_view_set_search_column (GTK_TREE_VIEW(app_data->tree_view), app_data->search_column + 1);

    GtkBindingSet *tv_binding_set = gtk_binding_set_by_class (GTK_TREE_VIEW_GET_CLASS(app_data->tree_view));
    g_signal_new ("hide-all-otps", G_TYPE_OBJECT, G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    gtk_binding_entry_add_signal (tv_binding_set, GDK_KEY_h, GDK_MOD1_MASK, "hide-all-otps", 0);

    // signal emitted when row is selected
    g_signal_connect (app_data->tree_view, "row-activated", G_CALLBACK(row_selected_cb), app_data);

    // signal emitted when CTRL+H is pressed
    g_signal_connect (app_data->tree_view, "hide-all-otps", G_CALLBACK(hide_all_otps_cb), app_data);

    // signal emitted when right-clicked on a row (shows edit/delete context menu)
    g_signal_connect(app_data->tree_view, "button-press-event", G_CALLBACK(on_treeview_button_press_event), app_data);

    g_object_unref (list_store);
}


void
update_model (AppData *app_data)
{
    if (app_data->tree_view != NULL) {
        GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model (app_data->tree_view));
        gtk_list_store_clear (store);
        add_data_to_model (app_data->db_data, store);
    }
}


void
row_selected_cb (GtkTreeView        *tree_view,
                 GtkTreePath        *path,
                 GtkTreeViewColumn  *column UNUSED,
                 gpointer            user_data)
{
    CAST_USER_DATA(AppData, app_data, user_data);
    if (app_data->is_reorder_active == FALSE) {
        GtkTreeModel *model = gtk_tree_view_get_model (tree_view);

        GtkTreeIter iter;
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
            g_application_send_notification (G_APPLICATION(gtk_window_get_application (GTK_WINDOW (app_data->main_window))), NOTIFICATION_ID,
                                             app_data->notification);
        }

        g_date_time_unref (now);
        g_free (otp_type);
        g_free (otp_value);
    }
}


void
reorder_db (AppData *app_data)
{
    // Iter through all rows. If the position in treeview is different from current_db_pos, then compute hash and add (hash,newpos) to the list
    GSList *nodes_order_slist = NULL;
    GtkTreeIter iter;
    guint current_db_pos;
    GtkTreeModel *model = gtk_tree_view_get_model (app_data->tree_view);

    gint slist_len = 0;
    gboolean valid = gtk_tree_model_get_iter_first (model, &iter);
    while (valid) {
        GtkTreePath *path = gtk_tree_model_get_path (model, &iter);
        gtk_tree_model_get (model, &iter, COLUMN_POSITION_IN_DB, &current_db_pos, -1);
        if (gtk_tree_path_get_indices (path)[0] != current_db_pos) {
            NodeInfo *node_info = g_new0 (NodeInfo, 1);
            json_t *obj = json_array_get (app_data->db_data->in_memory_json_data, current_db_pos);
            node_info->newpos = gtk_tree_path_get_indices (path)[0];
            node_info->hash = json_object_get_hash (obj);
            nodes_order_slist = g_slist_append (nodes_order_slist, g_memdup2 (node_info, sizeof (NodeInfo)));
            slist_len++;
            g_free (node_info);
        }
        gtk_tree_path_free (path);
        valid = gtk_tree_model_iter_next(model, &iter);
    }

    // move the reordered items to their new position in the database
    gsize index;
    json_t *obj;
    for (gint i = 0; i < slist_len; i++) {
        NodeInfo *ni = g_slist_nth_data (nodes_order_slist, i);
        json_array_foreach (app_data->db_data->in_memory_json_data, index, obj) {
            guint32 db_obj_hash = json_object_get_hash (obj);
            if (db_obj_hash == ni->hash) {
                // remove the obj from the current position...
                json_incref (obj);
                json_array_remove (app_data->db_data->in_memory_json_data, index);
                // ...and add it to the desired one
                json_array_insert (app_data->db_data->in_memory_json_data, ni->newpos, obj);
                json_decref (obj);
            }
        }
        g_free (ni);
    }

    // update the database and reload the changes
    GError *err = NULL;
    update_db (app_data->db_data, &err);
    if (err != NULL) {
        gchar *msg = g_strconcat ("[ERROR] Failed to update the db: ", err->message, NULL);
        show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
        g_free (msg);
        g_clear_error (&err);
        return;
    }
    reload_db (app_data->db_data, &err);
    if (err != NULL) {
        gchar *msg = g_strconcat ("[ERROR] Failed to reload the db: ", err->message, NULL);
        show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
        g_free (msg);
        g_clear_error (&err);
        return;
    }
    regenerate_model (app_data);

    g_slist_free (nodes_order_slist);
}


void
regenerate_model (AppData *app_data)
{
    update_model (app_data);
    g_slist_free_full (app_data->db_data->data_to_add, json_free);
    app_data->db_data->data_to_add = NULL;
}


static void
delete_row (AppData *app_data)
{
    g_return_if_fail (app_data->tree_view != NULL);

    GtkTreeModel *model = gtk_tree_view_get_model (app_data->tree_view);
    GtkTreeSelection *selection = gtk_tree_view_get_selection (GTK_TREE_VIEW(app_data->tree_view));
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected (selection, &model, &iter)) {
        show_message_dialog (app_data->main_window, "No row has been selected. Nothing will be deleted.", GTK_MESSAGE_ERROR);
        return;
    }

    gboolean delete_entry = FALSE;
    GtkWidget *del_diag = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "del_diag_id"));
    gtk_window_set_transient_for (GTK_WINDOW(del_diag), GTK_WINDOW(app_data->main_window));
    gint res = gtk_dialog_run (GTK_DIALOG(del_diag));
    switch (res) {
        case GTK_RESPONSE_YES:
            delete_entry = TRUE;
            break;
        case GTK_RESPONSE_NO:
        default:
            delete_entry = FALSE;
            break;
    }
    gtk_widget_hide (del_diag);

    if (delete_entry == FALSE) {
        return;
    }

    gint db_item_position_to_delete;
    gtk_tree_model_get (model, &iter, COLUMN_POSITION_IN_DB, &db_item_position_to_delete, -1);

    json_array_remove (app_data->db_data->in_memory_json_data, db_item_position_to_delete);
    gtk_list_store_remove (GTK_LIST_STORE(model), &iter);

    // json_array_remove shifts all items, so we have to take care of updating the real item's position in the database
    gint row_db_pos;
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    while (valid) {
        gtk_tree_model_get (model, &iter, COLUMN_POSITION_IN_DB, &row_db_pos, -1);
        if (row_db_pos > db_item_position_to_delete) {
            gint shifted_position = row_db_pos - 1;
            gtk_list_store_set (GTK_LIST_STORE(model), &iter, COLUMN_POSITION_IN_DB, shifted_position, -1);
        }
        valid = gtk_tree_model_iter_next(model, &iter);
    }

    GError *err = NULL;
    update_db (app_data->db_data, &err);
    if (err != NULL) {
        gchar *msg = g_strconcat ("The database update <b>FAILED</b>. The error message is:\n", err->message, NULL);
        show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
        g_free (msg);
    } else {
        reload_db (app_data->db_data, &err);
        if (err != NULL) {
            gchar *msg = g_strconcat ("The database update <b>FAILED</b>. The error message is:\n", err->message, NULL);
            show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
            g_free (msg);
        }
    }
}


static void
on_delete_activate (GtkMenuItem *menuitem UNUSED,
                    gpointer     user_data)
{
    CAST_USER_DATA(AppData, app_data, user_data);

    g_signal_handlers_disconnect_by_func (app_data->tree_view, row_selected_cb, app_data);

    // clear all active otps before proceeding to the deletion phase
    g_signal_emit_by_name (app_data->tree_view, "hide-all-otps");

    delete_row (app_data);

    // deletion is done, re-add the signal
    g_signal_connect (app_data->tree_view, "row-activated", G_CALLBACK(row_selected_cb), app_data);
}


static gboolean
on_treeview_button_press_event (GtkWidget      *treeview,
                                GdkEventButton *event,
                                gpointer        user_data)
{
    CAST_USER_DATA(AppData, app_data, user_data);
    if (event->type == GDK_BUTTON_PRESS && event->button == GDK_BUTTON_SECONDARY && !app_data->is_reorder_active) {
        GtkTreePath *path;
        GtkTreeSelection *selection = gtk_tree_view_get_selection (GTK_TREE_VIEW(treeview));
        if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(treeview), (gint)event->x, (gint)event->y, &path, NULL, NULL, NULL)) {
            gtk_tree_selection_select_path (selection, path);
            gtk_tree_path_free (path);

            GtkWidget *menu = gtk_menu_new ();
            GtkWidget *menu_item = gtk_menu_item_new_with_label ("Edit row");
            g_signal_connect(menu_item, "activate", G_CALLBACK (edit_row_cb), app_data);
            gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);

            menu_item = gtk_menu_item_new_with_label ("Delete row");
            g_signal_connect (menu_item, "activate", G_CALLBACK (on_delete_activate), app_data);
            gtk_menu_shell_append (GTK_MENU_SHELL(menu), menu_item);

            menu_item = gtk_menu_item_new_with_label ("Show QR code");
            g_signal_connect (menu_item, "activate", G_CALLBACK (show_qr_cb), app_data);
            gtk_menu_shell_append (GTK_MENU_SHELL(menu), menu_item);

            gtk_widget_show_all (menu);
            gtk_menu_popup_at_pointer (GTK_MENU(menu), (GdkEvent *)event);

            return TRUE;
        }
    }
    return FALSE;
}


static void
hide_all_otps_cb (GtkTreeView *tree_view,
                  gpointer     user_data)
{
    gtk_tree_model_foreach (GTK_TREE_MODEL(gtk_tree_view_get_model (tree_view)), clear_all_otps, user_data);
}


static gboolean
clear_all_otps (GtkTreeModel *model,
                GtkTreePath  *path UNUSED,
                GtkTreeIter  *iter,
                gpointer      user_data UNUSED)
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
    pjd->types = (gchar **)g_malloc0 ((array_len + 1)  * sizeof(gchar *));
    pjd->labels = (gchar **)g_malloc0 ((array_len + 1) * sizeof(gchar *));
    pjd->issuers = (gchar **)g_malloc0 ((array_len + 1) * sizeof(gchar *));
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

    set_json_data (db_data->in_memory_json_data, pjd);

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
add_column_with_attributes (GtkTreeView *tree_view,
                            const gchar *title,
                            gint         column_id,
                            gboolean     visible)
{
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new ();
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes (title, renderer, "text", column_id, NULL);
    gtk_tree_view_column_set_visible (column, visible);
    gtk_tree_view_append_column (tree_view, column);
}


static void
add_columns (GtkTreeView *tree_view)
{
    // Main columns
    add_column_with_attributes (tree_view, "Type", COLUMN_TYPE, TRUE);
    add_column_with_attributes (tree_view, "Account", COLUMN_ACC_LABEL, TRUE);
    add_column_with_attributes (tree_view, "Issuer", COLUMN_ACC_ISSUER, TRUE);
    add_column_with_attributes (tree_view, "OTP Value", COLUMN_OTP, TRUE);
    add_column_with_attributes (tree_view, "Validity", COLUMN_VALIDITY, TRUE);

    // Additional columns (hidden by default)
    add_column_with_attributes (tree_view, "Period", COLUMN_PERIOD, FALSE);
    add_column_with_attributes (tree_view, "Updated", COLUMN_UPDATED, FALSE);
    add_column_with_attributes (tree_view, "Less Than a Minute", COLUMN_LESS_THAN_A_MINUTE, FALSE);
    add_column_with_attributes (tree_view, "Position in Database", COLUMN_POSITION_IN_DB, FALSE);
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
