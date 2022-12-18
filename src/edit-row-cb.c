#include <gtk/gtk.h>
#include <gcrypt.h>
#include "imports.h"
#include "treeview.h"
#include "db-misc.h"
#include "get-builder.h"
#include "message-dialogs.h"
#include "gui-common.h"
#include "gquarks.h"
#include "common/common.h"

typedef struct edit_data_t {
    GtkListStore *list_store;
    GtkTreeIter iter;
    DatabaseData *db_data;
    gchar *current_label;
    gchar *new_label;
    gchar *current_issuer;
    gchar *new_issuer;
    gchar *current_secret;
    gchar *new_secret;
} EditData;

static void   show_edit_dialog                    (EditData   *edit_data,
                                                   AppData    *app_data);

static void   set_entry_editability               (GtkToolButton *btn,
                                                   gpointer       user_data);

static gchar *get_parse_and_set_data_from_entries (EditData    *edit_data,
                                                   GtkWidget   *lab_ck_btn,
                                                   GtkWidget   *new_lab_entry,
                                                   GtkWidget   *iss_ck_btn,
                                                   GtkWidget   *new_iss_entry,
                                                   GtkWidget   *sec_ck_btn,
                                                   GtkWidget   *new_sec_entry);

static void   set_data_in_lstore_and_json         (EditData    *edit_data);


void
edit_row_cb (GSimpleAction *simple    __attribute__((unused)),
             GVariant      *parameter __attribute__((unused)),
             gpointer       user_data)
{
    EditData *edit_data = g_new0 (EditData, 1);
    AppData *app_data = (AppData *)user_data;
    edit_data->db_data = app_data->db_data;

    GtkTreeModel *model = gtk_tree_view_get_model (app_data->tree_view);

    edit_data->list_store = GTK_LIST_STORE(model);

    if (gtk_tree_selection_get_selected (gtk_tree_view_get_selection (app_data->tree_view), &model, &edit_data->iter)) {
        gtk_tree_model_get (model, &edit_data->iter, COLUMN_ACC_LABEL, &edit_data->current_label, COLUMN_ACC_ISSUER, &edit_data->current_issuer, -1);
        show_edit_dialog (edit_data, app_data);
        g_free (edit_data->current_label);
        g_free (edit_data->current_issuer);
        gcry_free (edit_data->current_secret);
    }

    GError *err = NULL;
    update_and_reload_db (app_data, app_data->db_data, TRUE, &err);
    if (err != NULL && !g_error_matches (err, missing_file_gquark (), MISSING_FILE_CODE)) {
        show_message_dialog (app_data->main_window, err->message, GTK_MESSAGE_ERROR);
    }

    g_free (edit_data->new_label);
    g_free (edit_data->new_issuer);
    if (edit_data->new_secret != NULL) {
        gcry_free (edit_data->new_secret);
    }
    g_free (edit_data);
}


void
edit_row_cb_shortcut (GtkWidget *w __attribute__((unused)),
                      gpointer   user_data)
{
    edit_row_cb (NULL, NULL, user_data);
}


static void
show_edit_dialog (EditData *edit_data,
                  AppData  *app_data)
{
    GtkBuilder *builder = get_builder_from_partial_path (UI_PARTIAL_PATH);
    GtkWidget *diag = GTK_WIDGET (gtk_builder_get_object (builder, "edit_diag_id"));

    gtk_window_set_transient_for (GTK_WINDOW(diag), GTK_WINDOW(app_data->main_window));

    GtkWidget *new_lab_entry = GTK_WIDGET (gtk_builder_get_object (builder, "entry_newlabel_id"));
    GtkWidget *new_iss_entry = GTK_WIDGET (gtk_builder_get_object (builder, "entry_newissuer_id"));
    GtkWidget *new_sec_entry = GTK_WIDGET (gtk_builder_get_object (builder, "entry_newsec_id"));
    g_signal_connect (new_sec_entry, "icon-press", G_CALLBACK (icon_press_cb), NULL);

    if (edit_data->current_label != NULL) {
        gtk_entry_set_text (GTK_ENTRY(new_lab_entry), edit_data->current_label);
    }

    if (edit_data->current_issuer != NULL) {
        gtk_entry_set_text (GTK_ENTRY(new_iss_entry), edit_data->current_issuer);
        if (g_ascii_strcasecmp (edit_data->current_issuer, "steam") == 0) {
            gtk_widget_set_sensitive (new_iss_entry, FALSE);
        }
    }

    guint row_number = get_row_number_from_iter (edit_data->list_store, edit_data->iter);
    json_t *obj = json_array_get (edit_data->db_data->json_data, row_number);
    edit_data->current_secret = secure_strdup (json_string_value (json_object_get (obj, "secret")));
    if (edit_data->current_secret != NULL) {
        gtk_entry_set_text (GTK_ENTRY(new_sec_entry), edit_data->current_secret);
    }

    GtkWidget *lab_ck_btn = GTK_WIDGET (gtk_builder_get_object (builder, "label_check_id"));
    g_signal_connect (lab_ck_btn, "toggled", G_CALLBACK (set_entry_editability), new_lab_entry);

    GtkWidget *iss_ck_btn = GTK_WIDGET (gtk_builder_get_object (builder, "issuer_check_id"));
    g_signal_connect (iss_ck_btn, "toggled", G_CALLBACK (set_entry_editability), new_iss_entry);

    GtkWidget *sec_ck_btn = GTK_WIDGET (gtk_builder_get_object (builder, "secret_check_id"));
    g_signal_connect (sec_ck_btn, "toggled", G_CALLBACK (set_entry_editability), new_sec_entry);

    gchar *err_msg = NULL;
    gint res = gtk_dialog_run (GTK_DIALOG (diag));
    switch (res) {
        case GTK_RESPONSE_OK:
            err_msg = get_parse_and_set_data_from_entries (edit_data,
                                                           lab_ck_btn, new_lab_entry,
                                                           iss_ck_btn, new_iss_entry,
                                                           sec_ck_btn, new_sec_entry);
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


static void
set_entry_editability (GtkToolButton *btn __attribute__((unused)),
                       gpointer       user_data)
{
    gtk_editable_set_editable (GTK_EDITABLE (user_data), !gtk_editable_get_editable(user_data));
}


static gchar *
get_parse_and_set_data_from_entries (EditData    *edit_data,
                                     GtkWidget   *lab_ck_btn,
                                     GtkWidget   *new_lab_entry,
                                     GtkWidget   *iss_ck_btn,
                                     GtkWidget   *new_iss_entry,
                                     GtkWidget   *sec_ck_btn,
                                     GtkWidget   *new_sec_entry)
{
    edit_data->new_label = g_strdup (gtk_entry_get_text (GTK_ENTRY (new_lab_entry)));
    edit_data->new_issuer = g_strdup (gtk_entry_get_text (GTK_ENTRY (new_iss_entry)));
    edit_data->new_secret = secure_strdup (gtk_entry_get_text (GTK_ENTRY (new_sec_entry)));

    if (g_utf8_strlen (edit_data->new_issuer, -1) > 0) {
        if (!g_str_is_ascii (edit_data->new_issuer)) {
            return g_strdup ("Issuer entry must contain only ASCII characters.");
        }
    }

    if (g_utf8_strlen (edit_data->new_label, -1) < 1 || !g_str_is_ascii (edit_data->new_label)) {
        return g_strdup ("Label must not be empty and must contain only ASCII characters.");
    }

    if (!g_str_is_ascii (edit_data->new_secret) || g_utf8_strlen (edit_data->new_secret, -1) < 6) {
        return g_strdup ("The secret must be at least 6 characters long and must contain only ASCII characters.");
    }

    if (!gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON(lab_ck_btn)) || g_strcmp0 (edit_data->current_label, edit_data->new_label) == 0) {
        g_free (edit_data->new_label);
        edit_data->new_label = NULL;
    }
    if (!gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON(iss_ck_btn)) || g_strcmp0 (edit_data->current_issuer, edit_data->new_issuer) == 0) {
        g_free (edit_data->new_issuer);
        edit_data->new_issuer = NULL;
    }
    if (!gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON(sec_ck_btn)) || g_strcmp0 (edit_data->current_secret, edit_data->new_secret) == 0) {
        gcry_free (edit_data->new_secret);
        edit_data->new_secret = NULL;
    }
    set_data_in_lstore_and_json (edit_data);

    return NULL;
}


static void
set_data_in_lstore_and_json (EditData *edit_data)
{
    guint row_number = get_row_number_from_iter (edit_data->list_store, edit_data->iter);
    json_t *obj = json_array_get (edit_data->db_data->json_data, row_number);

    if (edit_data->new_label != NULL) {
        gtk_list_store_set (edit_data->list_store, &edit_data->iter, COLUMN_ACC_LABEL, edit_data->new_label, -1);
        json_object_set (obj, "label", json_string (edit_data->new_label));
    }
    if (edit_data->new_issuer != NULL) {
        gtk_list_store_set (edit_data->list_store, &edit_data->iter, COLUMN_ACC_ISSUER, edit_data->new_issuer, -1);
        json_object_set (obj, "issuer", json_string (edit_data->new_issuer));
    }
    if (edit_data->new_secret != NULL) {
        json_object_set (obj, "secret", json_string (edit_data->new_secret));
    }
}