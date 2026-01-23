#include <gtk/gtk.h>
#include <pango/pango.h>
#include <jansson.h>
#include "../common/macros.h"
#include "otpclient.h"
#include "liststore-misc.h"
#include "message-dialogs.h"
#include "edit-row-cb.h"
#include "show-qr-cb.h"
#include "gui-misc.h"


static const gchar *json_get_string_or_empty (json_t      *obj,
                                              const gchar *key);

static void     add_data_to_model  (DatabaseData   *db_data,
                                    GtkListStore   *store);

static void     add_columns        (GtkTreeView    *tree_view);

static void     add_validity_column (GtkTreeView   *tree_view);

static void     validity_cell_data_func (GtkTreeViewColumn *column,
                                         GtkCellRenderer   *renderer,
                                         GtkTreeModel      *model,
                                         GtkTreeIter       *iter,
                                         gpointer           user_data);

static GdkPixbuf *create_validity_pixbuf (guint validity,
                                          guint period);

static void     delete_row         (AppData        *app_data);

static void     hide_all_otps_cb   (GtkTreeView    *tree_view,
                                    gpointer        user_data);

static gboolean clear_all_otps     (GtkTreeModel   *model,
                                    GtkTreePath    *path,
                                    GtkTreeIter    *iter,
                                    gpointer        user_data);

static gboolean on_treeview_button_press_event (GtkWidget *treeview,
                                                GdkEventButton *event,
                                                gpointer user_data);

static gboolean filter_visible_func (GtkTreeModel *model,
                                     GtkTreeIter  *iter,
                                     gpointer      user_data);

static gboolean row_matches_query (GtkTreeModel *model,
                                   GtkTreeIter  *iter,
                                   const gchar  *query_folded);

static void     search_entry_changed_cb (GtkEntry *entry,
                                         gpointer  user_data);

static void     search_entry_activate_cb (GtkEntry *entry,
                                          gpointer  user_data);

static void     select_first_row (AppData *app_data);

static void     update_empty_state (AppData *app_data);

static gboolean get_liststore_iter_from_path (AppData     *app_data,
                                              GtkTreePath *path,
                                              GtkTreeIter *iter);


void
create_treeview (AppData *app_data)
{
    app_data->tree_view = GTK_TREE_VIEW(gtk_builder_get_object (app_data->builder, "treeview_id"));

    app_data->list_store = gtk_list_store_new (NUM_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                               G_TYPE_UINT, G_TYPE_UINT, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_INT);

    add_columns (app_data->tree_view);

    add_data_to_model (app_data->db_data, app_data->list_store);

    app_data->filter_model = GTK_TREE_MODEL_FILTER(gtk_tree_model_filter_new (GTK_TREE_MODEL(app_data->list_store), NULL));
    gtk_tree_model_filter_set_visible_func (app_data->filter_model, filter_visible_func, app_data, NULL);

    gtk_tree_view_set_model (app_data->tree_view, GTK_TREE_MODEL(app_data->filter_model));

    app_data->search_entry = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "search_entry_id"));
    if (app_data->search_entry != NULL) {
        gtk_tree_view_set_search_entry (GTK_TREE_VIEW(app_data->tree_view), GTK_ENTRY(app_data->search_entry));
        g_signal_connect (app_data->search_entry, "changed", G_CALLBACK(search_entry_changed_cb), app_data);
        g_signal_connect (app_data->search_entry, "activate", G_CALLBACK(search_entry_activate_cb), app_data);
    }

    app_data->list_stack = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "list_stack_id"));

    GtkBindingSet *tv_binding_set = gtk_binding_set_by_class (GTK_TREE_VIEW_GET_CLASS(app_data->tree_view));
    g_signal_new ("hide-all-otps", G_TYPE_OBJECT, G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    gtk_binding_entry_add_signal (tv_binding_set, GDK_KEY_h, GDK_MOD1_MASK, "hide-all-otps", 0);

    // signal emitted when row is selected
    g_signal_connect (app_data->tree_view, "row-activated", G_CALLBACK(row_selected_cb), app_data);

    // signal emitted when CTRL+H is pressed
    g_signal_connect (app_data->tree_view, "hide-all-otps", G_CALLBACK(hide_all_otps_cb), app_data);

    // signal emitted when right-clicked on a row (shows edit/delete context menu)
    g_signal_connect(app_data->tree_view, "button-press-event", G_CALLBACK(on_treeview_button_press_event), app_data);

    select_first_row (app_data);
    update_empty_state (app_data);
}


void
update_model (AppData *app_data)
{
    if (app_data->tree_view != NULL) {
        GtkListStore *store = app_data->list_store;
        if (store == NULL) {
            return;
        }
        gtk_list_store_clear (store);
        add_data_to_model (app_data->db_data, store);
        if (app_data->filter_model != NULL) {
            gtk_tree_model_filter_refilter (app_data->filter_model);
        }
    }
    update_empty_state (app_data);
}


void
row_selected_cb (GtkTreeView        *tree_view UNUSED,
                 GtkTreePath        *path,
                 GtkTreeViewColumn  *column UNUSED,
                 gpointer            user_data)
{
    CAST_USER_DATA(AppData, app_data, user_data);
    if (app_data->is_reorder_active == FALSE) {
        GtkTreeIter iter;
        if (!get_liststore_iter_from_path (app_data, path, &iter)) {
            return;
        }

        gchar *otp_type = NULL;
        gchar *otp_value = NULL;
        gtk_tree_model_get (GTK_TREE_MODEL(app_data->list_store), &iter,
                            COLUMN_TYPE, &otp_type,
                            COLUMN_OTP, &otp_value,
                            -1);

        GDateTime *now = g_date_time_new_now_local ();
        GTimeSpan diff = g_date_time_difference (now, app_data->db_data->last_hotp_update);
        gboolean should_update = (otp_value == NULL || g_utf8_strlen (otp_value, -1) <= 3);
        if (!should_update && otp_type != NULL && g_ascii_strcasecmp (otp_type, "HOTP") == 0) {
            should_update = (diff >= G_USEC_PER_SEC * HOTP_RATE_LIMIT_IN_SEC);
        }

        if (should_update) {
            set_otp (app_data->list_store, iter, app_data);
            g_free (otp_value);
            otp_value = NULL;
            gtk_tree_model_get (GTK_TREE_MODEL(app_data->list_store), &iter,
                                COLUMN_OTP, &otp_value,
                                -1);
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
    GtkTreeModel *model = GTK_TREE_MODEL(app_data->list_store);

    gint slist_len = 0;
    gboolean valid = gtk_tree_model_get_iter_first (model, &iter);
    while (valid) {
        GtkTreePath *path = gtk_tree_model_get_path (model, &iter);
        if (path == NULL) {
            valid = gtk_tree_model_iter_next(model, &iter);
            continue;
        }
        gtk_tree_model_get (model, &iter, COLUMN_POSITION_IN_DB, &current_db_pos, -1);
        gint *indices = gtk_tree_path_get_indices (path);
        if (indices != NULL && indices[0] != (gint)current_db_pos) {
            NodeInfo *node_info = g_new0 (NodeInfo, 1);
            json_t *obj = json_array_get (app_data->db_data->in_memory_json_data, current_db_pos);
            node_info->newpos = indices[0];
            node_info->hash = json_object_get_hash (obj);
            nodes_order_slist = g_slist_append (nodes_order_slist, node_info);
            slist_len++;
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

    g_slist_free_full (nodes_order_slist, g_free);
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

    GtkListStore *list_store = NULL;
    GtkTreeIter iter;
    if (!get_selected_liststore_iter (app_data, &list_store, &iter)) {
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
    gtk_tree_model_get (GTK_TREE_MODEL(list_store), &iter, COLUMN_POSITION_IN_DB, &db_item_position_to_delete, -1);

    json_array_remove (app_data->db_data->in_memory_json_data, db_item_position_to_delete);
    gtk_list_store_remove (list_store, &iter);

    // json_array_remove shifts all items, so we have to take care of updating the real item's position in the database
    gint row_db_pos;
    gboolean valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL(list_store), &iter);
    while (valid) {
        gtk_tree_model_get (GTK_TREE_MODEL(list_store), &iter, COLUMN_POSITION_IN_DB, &row_db_pos, -1);
        if (row_db_pos > db_item_position_to_delete) {
            gint shifted_position = row_db_pos - 1;
            gtk_list_store_set (list_store, &iter, COLUMN_POSITION_IN_DB, shifted_position, -1);
        }
        valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(list_store), &iter);
    }

    GError *err = NULL;
    update_db (app_data->db_data, &err);
    if (err != NULL) {
        gchar *msg = g_strconcat ("The database update <b>FAILED</b>. The error message is:\n", err->message, NULL);
        show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
        g_free (msg);
        g_clear_error (&err);
    } else {
        reload_db (app_data->db_data, &err);
        if (err != NULL) {
            gchar *msg = g_strconcat ("The database update <b>FAILED</b>. The error message is:\n", err->message, NULL);
            show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
            g_free (msg);
            g_clear_error (&err);
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
            g_signal_connect (menu_item, "activate", G_CALLBACK (edit_row_cb), app_data);
            gtk_menu_shell_append (GTK_MENU_SHELL(menu), menu_item);

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
hide_all_otps_cb (GtkTreeView *tree_view UNUSED,
                  gpointer     user_data)
{
    CAST_USER_DATA(AppData, app_data, user_data);
    if (app_data->list_store == NULL) {
        return;
    }
    gtk_tree_model_foreach (GTK_TREE_MODEL(app_data->list_store), clear_all_otps, user_data);
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


static gboolean
filter_visible_func (GtkTreeModel *model,
                     GtkTreeIter  *iter,
                     gpointer      user_data)
{
    CAST_USER_DATA(AppData, app_data, user_data);
    if (app_data->search_entry == NULL) {
        return TRUE;
    }

    const gchar *query = gtk_entry_get_text (GTK_ENTRY(app_data->search_entry));
    if (query == NULL || *query == '\0') {
        return TRUE;
    }

    gchar *query_folded = g_utf8_strdown (query, -1);
    gboolean match = row_matches_query (model, iter, query_folded);
    g_free (query_folded);

    return match;
}


static gboolean
row_matches_query (GtkTreeModel *model,
                   GtkTreeIter  *iter,
                   const gchar  *query_folded)
{
    gchar *type = NULL;
    gchar *label = NULL;
    gchar *issuer = NULL;
    gtk_tree_model_get (model, iter,
                        COLUMN_TYPE, &type,
                        COLUMN_ACC_LABEL, &label,
                        COLUMN_ACC_ISSUER, &issuer,
                        -1);

    gboolean match = FALSE;
    if (type != NULL) {
        gchar *type_folded = g_utf8_strdown (type, -1);
        match = (g_strstr_len (type_folded, -1, query_folded) != NULL);
        g_free (type_folded);
    }

    if (!match && label != NULL) {
        gchar *label_folded = g_utf8_strdown (label, -1);
        match = (g_strstr_len (label_folded, -1, query_folded) != NULL);
        g_free (label_folded);
    }

    if (!match && issuer != NULL) {
        gchar *issuer_folded = g_utf8_strdown (issuer, -1);
        match = (g_strstr_len (issuer_folded, -1, query_folded) != NULL);
        g_free (issuer_folded);
    }

    g_free (type);
    g_free (label);
    g_free (issuer);

    return match;
}

static void
search_entry_changed_cb (GtkEntry *entry UNUSED,
                         gpointer  user_data)
{
    CAST_USER_DATA(AppData, app_data, user_data);
    if (app_data->filter_model != NULL) {
        gtk_tree_model_filter_refilter (app_data->filter_model);
    }
    select_first_row (app_data);
    update_empty_state (app_data);
}

static void
search_entry_activate_cb (GtkEntry *entry UNUSED,
                          gpointer  user_data)
{
    CAST_USER_DATA(AppData, app_data, user_data);
    GtkTreeModel *model = GTK_TREE_MODEL(app_data->filter_model);
    GtkTreeSelection *selection = gtk_tree_view_get_selection (app_data->tree_view);
    if (model == NULL) {
        return;
    }

    if (gtk_tree_selection_count_selected_rows (selection) == 0) {
        select_first_row (app_data);
    }

    GList *paths = gtk_tree_selection_get_selected_rows (selection, &model);
    if (paths != NULL) {
        GtkTreePath *path = g_list_first (paths)->data;
        GtkTreeViewColumn *column = gtk_tree_view_get_column (app_data->tree_view, 0);
        gtk_tree_view_row_activated (app_data->tree_view, path, column);
        g_list_free_full (paths, (GDestroyNotify)gtk_tree_path_free);
    }
}

static void
select_first_row (AppData *app_data)
{
    if (app_data->filter_model == NULL) {
        return;
    }
    GtkTreeIter iter;
    GtkTreeModel *model = GTK_TREE_MODEL(app_data->filter_model);
    GtkTreeSelection *selection = gtk_tree_view_get_selection (app_data->tree_view);
    gtk_tree_selection_unselect_all (selection);
    if (gtk_tree_model_get_iter_first (model, &iter)) {
        GtkTreePath *path = gtk_tree_model_get_path (model, &iter);
        gtk_tree_selection_select_path (selection, path);
        gtk_tree_view_scroll_to_cell (app_data->tree_view, path, NULL, FALSE, 0.0f, 0.0f);
        gtk_tree_path_free (path);
    }
}

static void
update_empty_state (AppData *app_data)
{
    if (app_data->list_stack == NULL || app_data->filter_model == NULL) {
        return;
    }

    GtkTreeModel *model = GTK_TREE_MODEL(app_data->filter_model);
    gint rows = gtk_tree_model_iter_n_children (model, NULL);
    gtk_stack_set_visible_child_name (GTK_STACK(app_data->list_stack), rows > 0 ? "list" : "empty");
}

static gboolean
get_liststore_iter_from_path (AppData     *app_data,
                              GtkTreePath *path,
                              GtkTreeIter *iter)
{
    GtkTreeModel *model = gtk_tree_view_get_model (app_data->tree_view);
    GtkTreeIter view_iter;
    if (!gtk_tree_model_get_iter (model, &view_iter, path)) {
        return FALSE;
    }

    if (GTK_IS_TREE_MODEL_FILTER (model)) {
        gtk_tree_model_filter_convert_iter_to_child_iter (GTK_TREE_MODEL_FILTER(model), iter, &view_iter);
        return TRUE;
    }

    *iter = view_iter;
    return TRUE;
}


static void
add_data_to_model (DatabaseData *db_data,
                   GtkListStore *store)
{
    if (db_data == NULL || store == NULL || db_data->in_memory_json_data == NULL) {
        return;
    }

    gsize array_len = json_array_size (db_data->in_memory_json_data);
    for (guint i = 0; i < array_len; i++) {
        json_t *obj = json_array_get (db_data->in_memory_json_data, i);
        const gchar *type = json_get_string_or_empty (obj, "type");
        const gchar *label = json_get_string_or_empty (obj, "label");
        const gchar *issuer = json_get_string_or_empty (obj, "issuer");
        json_t *period_value = json_object_get (obj, "period");
        gint period = json_is_integer (period_value) ? (gint)json_integer_value (period_value) : 0;

        GtkTreeIter iter;
        gtk_list_store_append (store, &iter);
        gtk_list_store_set (store, &iter,
                            COLUMN_TYPE, type,
                            COLUMN_ACC_LABEL, label,
                            COLUMN_ACC_ISSUER, issuer,
                            COLUMN_PERIOD, period,
                            COLUMN_UPDATED, FALSE,
                            COLUMN_LESS_THAN_A_MINUTE, FALSE,
                            COLUMN_POSITION_IN_DB, i,
                            -1);
    }
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
    gtk_tree_view_column_set_resizable (column, TRUE);
    if (column_id == COLUMN_ACC_LABEL || column_id == COLUMN_ACC_ISSUER) {
        gtk_tree_view_column_set_expand (column, TRUE);
        g_object_set (renderer, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    }
    if (column_id == COLUMN_OTP) {
        g_object_set (renderer,
                      "family", "monospace",
                      "weight", PANGO_WEIGHT_NORMAL,
                      "xalign", 0.5,
                      NULL);
        gtk_tree_view_column_set_min_width (column, 120);
    }
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
    add_validity_column (tree_view);

    // Additional columns (hidden by default)
    add_column_with_attributes (tree_view, "Period", COLUMN_PERIOD, FALSE);
    add_column_with_attributes (tree_view, "Updated", COLUMN_UPDATED, FALSE);
    add_column_with_attributes (tree_view, "Less Than a Minute", COLUMN_LESS_THAN_A_MINUTE, FALSE);
    add_column_with_attributes (tree_view, "Position in Database", COLUMN_POSITION_IN_DB, FALSE);
}


static void
add_validity_column (GtkTreeView *tree_view)
{
    GtkCellRenderer *renderer = gtk_cell_renderer_pixbuf_new ();
    GtkTreeViewColumn *column = gtk_tree_view_column_new ();
    gtk_tree_view_column_set_title (column, "Validity");
    gtk_tree_view_column_pack_start (column, renderer, TRUE);
    gtk_tree_view_column_set_resizable (column, TRUE);
    gtk_tree_view_column_set_min_width (column, 40);
    gtk_tree_view_column_set_cell_data_func (column, renderer, validity_cell_data_func, NULL, NULL);
    gtk_tree_view_append_column (tree_view, column);
}


static void
validity_cell_data_func (GtkTreeViewColumn *column UNUSED,
                         GtkCellRenderer   *renderer,
                         GtkTreeModel      *model,
                         GtkTreeIter       *iter,
                         gpointer           user_data UNUSED)
{
    gchar *otp_type = NULL;
    gchar *otp_value = NULL;
    guint validity = 0;
    guint period = 0;

    gtk_tree_model_get (model, iter,
                        COLUMN_TYPE, &otp_type,
                        COLUMN_OTP, &otp_value,
                        COLUMN_VALIDITY, &validity,
                        COLUMN_PERIOD, &period,
                        -1);

    if (otp_value != NULL && g_utf8_strlen (otp_value, -1) > 4 && otp_type != NULL
        && g_ascii_strcasecmp (otp_type, "TOTP") == 0 && period > 0) {
        GdkPixbuf *pixbuf = create_validity_pixbuf (validity, period);
        g_object_set (renderer,
                      "pixbuf", pixbuf,
                      NULL);
        if (pixbuf != NULL) {
            g_object_unref (pixbuf);
        }
    } else {
        g_object_set (renderer,
                      "pixbuf", NULL,
                      NULL);
    }

    g_free (otp_type);
    g_free (otp_value);
}


static GdkPixbuf *
create_validity_pixbuf (guint validity,
                        guint period)
{
    const gint size = 18;
    cairo_surface_t *surface = cairo_image_surface_create (CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create (surface);
    gdouble center = size / 2.0;
    gdouble radius = size / 2.0;
    gdouble fraction = 0.0;

    if (period > 0) {
        fraction = (gdouble)validity / (gdouble)period;
    }

    cairo_set_source_rgba (cr, 0.75, 0.75, 0.75, 0.25);
    cairo_arc (cr, center, center, radius, 0, 2 * G_PI);
    cairo_fill (cr);

    if (validity <= 5) {
        cairo_set_source_rgba (cr, 0.85, 0.35, 0.25, 1.0);
    } else {
        cairo_set_source_rgba (cr, 0.20, 0.65, 0.35, 1.0);
    }
    cairo_move_to (cr, center, center);
    cairo_arc (cr, center, center, radius, -G_PI / 2.0, -G_PI / 2.0 + (2 * G_PI * fraction));
    cairo_close_path (cr);
    cairo_fill (cr);

    GdkPixbuf *pixbuf = gdk_pixbuf_get_from_surface (surface, 0, 0, size, size);

    cairo_destroy (cr);
    cairo_surface_destroy (surface);

    return pixbuf;
}


static const gchar *
json_get_string_or_empty (json_t      *obj,
                          const gchar *key)
{
    if (obj == NULL || key == NULL) {
        return "";
    }

    json_t *value = json_object_get (obj, key);
    const gchar *string_value = json_string_value (value);
    return string_value != NULL ? string_value : "";
}
