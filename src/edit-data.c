#include <gtk/gtk.h>
#include "imports.h"
#include "treeview.h"
#include "get-builder.h"
#include "message-dialogs.h"
#include "common.h"
#include "gquarks.h"

typedef struct _edit_data_t {
    GtkListStore *list_store;
    GtkTreeIter iter;
    DatabaseData *db_data;
} EditData;

static void show_edit_dialog (EditData *edit_data, ImportData *import_data, gchar *acc_lab, gchar *acc_iss);

static gchar *get_parse_and_set_data_from_entries (EditData *edit_data, GtkWidget *new_lab_entry, GtkWidget *new_iss_entry);

static void set_data_in_lstore_and_json (EditData *edit_data, const gchar *label, const gchar *issuer);


void
edit_selected_rows (GSimpleAction *simple    __attribute__((unused)),
                    GVariant      *parameter __attribute__((unused)),
                    gpointer       user_data)
{
    EditData *edit_data = g_new0 (EditData, 1);
    ImportData *import_data = (ImportData *)user_data;
    edit_data->db_data = import_data->db_data;
    edit_data->list_store = g_object_get_data (G_OBJECT (import_data->main_window), "lstore");

    gboolean valid, is_active;
    gchar *iss, *lab;

    valid = gtk_tree_model_get_iter_first (GTK_TREE_MODEL (edit_data->list_store), &edit_data->iter);
    while (valid) {
        gtk_tree_model_get (GTK_TREE_MODEL (edit_data->list_store), &edit_data->iter, COLUMN_BOOLEAN, &is_active, -1);

        if (is_active) {
            gtk_tree_model_get (GTK_TREE_MODEL (edit_data->list_store), &edit_data->iter, COLUMN_ACC_LABEL, &lab, -1);
            gtk_tree_model_get (GTK_TREE_MODEL (edit_data->list_store), &edit_data->iter, COLUMN_ACC_ISSUER, &iss, -1);
            show_edit_dialog (edit_data, import_data, lab, iss);
            g_free (iss);
            g_free (lab);
        }

        valid = gtk_tree_model_iter_next (GTK_TREE_MODEL (edit_data->list_store), &edit_data->iter);
    }

    GError *err = NULL;
    update_and_reload_db (edit_data->db_data, edit_data->list_store, TRUE, &err);
    if (err != NULL && !g_error_matches (err, missing_file_gquark (), MISSING_FILE_CODE)) {
        show_message_dialog (import_data->main_window, err->message, GTK_MESSAGE_ERROR);
    }
    g_free (edit_data);
}


static void
show_edit_dialog (EditData *edit_data, ImportData *import_data, gchar *acc_lab, gchar *acc_iss)
{
    GtkBuilder *builder = get_builder_from_partial_path ("share/otpclient/edit-diag.ui");
    GtkWidget *diag = GTK_WIDGET (gtk_builder_get_object (builder, "edit_diag_id"));
    gtk_window_set_transient_for (GTK_WINDOW (diag), GTK_WINDOW (import_data->main_window));

    GtkWidget *cur_lab_entry = GTK_WIDGET (gtk_builder_get_object (builder, "cur_label_entry"));
    GtkWidget *cur_iss_entry = GTK_WIDGET (gtk_builder_get_object (builder, "cur_iss_entry"));
    if (cur_lab_entry != NULL) {
        gtk_entry_set_text (GTK_ENTRY (cur_lab_entry), acc_lab);
    }
    if (cur_iss_entry != NULL) {
        gtk_entry_set_text (GTK_ENTRY (cur_iss_entry), acc_iss);
    }

    GtkWidget *new_lab_entry = GTK_WIDGET (gtk_builder_get_object (builder, "new_label_entry"));
    GtkWidget *new_iss_entry = GTK_WIDGET (gtk_builder_get_object (builder, "new_iss_entry"));

    gchar *err_msg = NULL;
    gint res = gtk_dialog_run (GTK_DIALOG (diag));
    switch (res) {
        case GTK_RESPONSE_OK:
            err_msg = get_parse_and_set_data_from_entries (edit_data, new_lab_entry, new_iss_entry);
            if (err_msg != NULL) {
                show_message_dialog (import_data->main_window, err_msg, GTK_MESSAGE_ERROR);
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

    if (!g_utf8_validate (new_label, -1, NULL) || !g_utf8_validate (new_issuer, -1, NULL)) {
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