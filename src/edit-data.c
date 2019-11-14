#include <gtk/gtk.h>
#include "imports.h"
#include "treeview.h"
#include "db-misc.h"
#include "get-builder.h"
#include "message-dialogs.h"
#include "gui-common.h"
#include "gquarks.h"

typedef struct _edit_data_t {
    GtkListStore *list_store;
    GtkTreeIter iter;
    DatabaseData *db_data;
} EditData;

static void show_edit_dialog (EditData *edit_data, AppData *app_data, gchar *current_label, gchar *current_issuer);

static gchar *get_parse_and_set_data_from_entries (EditData *edit_data, GtkWidget *new_lab_entry, GtkWidget *new_iss_entry);

static void set_data_in_lstore_and_json (EditData *edit_data, const gchar *label, const gchar *issuer);


void
edit_selected_row_cb (GSimpleAction *simple    __attribute__((unused)),
                      GVariant      *parameter __attribute__((unused)),
                      gpointer       user_data)
{
    EditData *edit_data = g_new0 (EditData, 1);
    AppData *app_data = (AppData *)user_data;
    edit_data->db_data = app_data->db_data;

    GtkTreeModel *model = gtk_tree_view_get_model (app_data->tree_view);

    edit_data->list_store = GTK_LIST_STORE(model);

    gchar *current_label, *current_issuer;
    if (gtk_tree_selection_get_selected (gtk_tree_view_get_selection (app_data->tree_view), &model, &edit_data->iter)) {
        gtk_tree_model_get (model, &edit_data->iter, COLUMN_ACC_LABEL, &current_label, COLUMN_ACC_ISSUER, &current_issuer, -1);
        show_edit_dialog (edit_data, app_data, current_label, current_issuer);
        g_free (current_label);
        g_free (current_issuer);
    }

    GError *err = NULL;
    update_and_reload_db (app_data, app_data->db_data, TRUE, &err);
    if (err != NULL && !g_error_matches (err, missing_file_gquark (), MISSING_FILE_CODE)) {
        show_message_dialog (app_data->main_window, err->message, GTK_MESSAGE_ERROR);
    }
    g_free (edit_data);
}


static void
show_edit_dialog (EditData *edit_data, AppData *app_data, gchar *current_label, gchar *current_issuer)
{
    GtkBuilder *builder = get_builder_from_partial_path (UI_PARTIAL_PATH);
    GtkWidget *diag = GTK_WIDGET (gtk_builder_get_object (builder, "edit_diag_id"));

    gtk_window_set_transient_for (GTK_WINDOW(diag), GTK_WINDOW(app_data->main_window));

    GtkWidget *cur_lab_entry = GTK_WIDGET (gtk_builder_get_object (builder, "cur_label_entry"));
    GtkWidget *cur_iss_entry = GTK_WIDGET (gtk_builder_get_object (builder, "cur_iss_entry"));
    if (cur_lab_entry != NULL) {
        gtk_entry_set_text (GTK_ENTRY (cur_lab_entry), current_label);
    }
    if (cur_iss_entry != NULL) {
        gtk_entry_set_text (GTK_ENTRY (cur_iss_entry), current_issuer);
    }

    GtkWidget *new_lab_entry = GTK_WIDGET (gtk_builder_get_object (builder, "entry_newlabel_id"));
    GtkWidget *new_iss_entry = GTK_WIDGET (gtk_builder_get_object (builder, "entry_newissuer_id"));

    if (current_label != NULL) {
        gtk_entry_set_text (GTK_ENTRY(new_lab_entry), current_label);
    }
    if (current_issuer != NULL) {
        gtk_entry_set_text (GTK_ENTRY(new_iss_entry), current_issuer);
    }

    if (current_issuer != NULL && g_ascii_strcasecmp (current_issuer, "steam") == 0) {
        gtk_widget_set_sensitive (new_iss_entry, FALSE);
    }

    gchar *err_msg = NULL;
    gint res = gtk_dialog_run (GTK_DIALOG (diag));
    switch (res) {
        case GTK_RESPONSE_OK:
            err_msg = get_parse_and_set_data_from_entries (edit_data, new_lab_entry, new_iss_entry);
            if (err_msg != NULL) {
                show_message_dialog (app_data->main_window, err_msg, GTK_MESSAGE_ERROR);
                g_free (err_msg);
            }
            break;
        case GTK_RESPONSE_CANCEL:
        default:
            break;
    }

    gtk_widget_destroy (diag);
    g_object_unref (builder);
}


static gchar *
get_parse_and_set_data_from_entries (EditData *edit_data, GtkWidget *new_lab_entry, GtkWidget *new_iss_entry)
{
    const gchar *new_label = gtk_entry_get_text (GTK_ENTRY (new_lab_entry));
    const gchar *new_issuer = gtk_entry_get_text (GTK_ENTRY (new_iss_entry));

    if (!g_str_is_ascii (new_label) || !g_str_is_ascii (new_issuer)) {
        return g_strdup ("Only ASCII characters are supported at the moment.");
    }

    if (g_utf8_strlen (new_label, -1) == 0) {
        return g_strdup ("Label must not be empty");
    }

    set_data_in_lstore_and_json (edit_data, new_label, new_issuer);

    return NULL;
}


static void
set_data_in_lstore_and_json (EditData *edit_data, const gchar *label, const gchar *issuer)
{
    gtk_list_store_set (edit_data->list_store, &edit_data->iter,
                        COLUMN_ACC_LABEL, label,
                        COLUMN_ACC_ISSUER, issuer,
                        -1);

    guint row_number = get_row_number_from_iter (edit_data->list_store, edit_data->iter);
    json_t *obj = json_array_get (edit_data->db_data->json_data, row_number);
    json_object_set (obj, "label", json_string (label));
    json_object_set (obj, "issuer", json_string (issuer));
}