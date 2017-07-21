#include <gtk/gtk.h>
#include "kf-misc.h"
#include "otpclient.h"
#include "common.h"

typedef struct _widgets {
    GtkWidget *dialog;
    GtkWidget *grid;
    GArray *acc_entry;
    GArray *key_entry;
    gint grid_top;
} Widgets;

static GtkWidget *create_scrolled_window (GtkWidget *content_area);

static void setup_header_bar (Widgets *widgets);

static void parse_user_data (Widgets *widgets, UpdateData *kf_data);

static void add_entry_cb (GtkWidget *btn, gpointer user_data);

static void update_grid (Widgets *widgets);

static void del_entry_cb (GtkWidget *btn, gpointer user_data);

static void cleanup_widgets (Widgets *widgets);


int
add_data_dialog (GtkWidget *main_win, UpdateData *kf_data)
{
    Widgets *widgets = g_new0 (Widgets, 1);
    widgets->acc_entry = g_array_new (FALSE, FALSE, sizeof (GtkWidget *));
    widgets->key_entry = g_array_new (FALSE, FALSE, sizeof (GtkWidget *));
    widgets->grid_top = 0;

    widgets->dialog = gtk_dialog_new_with_buttons ("Add Data to Database",
                                                   GTK_WINDOW (main_win),
                                                   GTK_DIALOG_DESTROY_WITH_PARENT, "OK",
                                                   GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL,
                                                   NULL);

    gtk_widget_set_size_request (widgets->dialog, 400, 400);

    GtkWidget *content_area = gtk_dialog_get_content_area (GTK_DIALOG (widgets->dialog));

    GtkWidget *acc_label = gtk_label_new ("Account Name");
    GtkWidget *key_label = gtk_label_new ("Secret Key");

    GtkWidget *scrolled_win = create_scrolled_window (content_area);

    setup_header_bar (widgets);

    widgets->grid = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (widgets->grid), 3);
    gtk_grid_set_row_spacing (GTK_GRID (widgets->grid), 3);
    gtk_container_add (GTK_CONTAINER (scrolled_win), widgets->grid);
    gtk_grid_attach (GTK_GRID (widgets->grid), acc_label, 0, widgets->grid_top++, 4, 1);
    gtk_grid_attach_next_to (GTK_GRID (widgets->grid), key_label, acc_label, GTK_POS_RIGHT, 4, 1);

    add_entry_cb (NULL, widgets);

    gint result = gtk_dialog_run (GTK_DIALOG (widgets->dialog));
    switch (result) {
        case GTK_RESPONSE_OK:
            parse_user_data (widgets, kf_data);
            update_kf (kf_data, TRUE);
            g_hash_table_remove_all (kf_data->data_to_add);
            g_hash_table_unref (kf_data->data_to_add);
            break;
        case GTK_RESPONSE_CANCEL:
        default:
            break;
    }
    gtk_widget_destroy (widgets->dialog);
    cleanup_widgets (widgets);

    return 0;
}


static GtkWidget *
create_scrolled_window (GtkWidget *content_area)
{
    GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add (GTK_CONTAINER (content_area), vbox);

    GtkWidget *sw = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (sw), GTK_SHADOW_ETCHED_IN);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start (GTK_BOX (vbox), sw, TRUE, TRUE, 0);

    g_object_set (sw, "expand", TRUE, NULL);

    return sw;
}


static void
setup_header_bar (Widgets *widgets)
{
    GtkWidget *header_bar = create_header_bar ("Add a new account");
    GtkWidget *box = create_box_with_buttons ("add_btn_dialog", "del_btn_dialog");
    gtk_header_bar_pack_start (GTK_HEADER_BAR (header_bar), box);
    gtk_window_set_titlebar (GTK_WINDOW (widgets->dialog), header_bar);
    g_signal_connect (find_widget (box, "add_btn_dialog"), "clicked", G_CALLBACK (add_entry_cb), widgets);
    g_signal_connect (find_widget (box, "del_btn_dialog"), "clicked", G_CALLBACK (del_entry_cb), widgets);
}


static void
add_entry_cb (GtkWidget *btn __attribute__((__unused__)),
              gpointer user_data)
{
    Widgets *widgets = (Widgets *)user_data;
    GtkWidget *acc_entry = gtk_entry_new ();
    GtkWidget *key_entry = gtk_entry_new ();
    g_array_append_val (widgets->acc_entry, acc_entry);
    g_array_append_val (widgets->key_entry, key_entry);
    set_icon_to_entry (g_array_index (widgets->key_entry, GtkWidget *, widgets->key_entry->len - 1), "dialog-password-symbolic", "Show password");
    update_grid (widgets);
}


static void
update_grid (Widgets *widgets)
{
    GtkWidget *acc_entry_to_add = g_array_index (widgets->acc_entry, GtkWidget *, widgets->acc_entry->len - 1);
    GtkWidget *key_entry_to_add = g_array_index (widgets->key_entry, GtkWidget *, widgets->key_entry->len - 1);
    gtk_grid_attach (GTK_GRID (widgets->grid), acc_entry_to_add, 0, widgets->grid_top++, 4, 1);
    gtk_grid_attach_next_to (GTK_GRID (widgets->grid), key_entry_to_add, acc_entry_to_add, GTK_POS_RIGHT, 4, 1);
    gtk_widget_show_all (widgets->dialog);
}


static void
del_entry_cb (GtkWidget *btn __attribute__((__unused__)),
              gpointer user_data)
{
    Widgets *widgets = (Widgets *)user_data;
    guint current_position = widgets->acc_entry->len - 1;
    if (current_position == 0) //at least one row has to remain
        return;

    gtk_widget_destroy (g_array_index (widgets->acc_entry, GtkWidget *, current_position));
    gtk_widget_destroy (g_array_index (widgets->key_entry, GtkWidget *, current_position));
    g_array_remove_index (widgets->acc_entry, current_position);
    g_array_remove_index (widgets->key_entry, current_position);
    widgets->grid_top--;
}


static void
parse_user_data (Widgets *widgets, UpdateData *kf_data)
{
    kf_data->data_to_add = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
    gint i = 0;
    while (i < widgets->acc_entry->len) {
        const gchar *acc_name = gtk_entry_get_text (GTK_ENTRY (g_array_index (widgets->acc_entry, GtkWidget *, i)));
        const gchar *acc_key = gtk_entry_get_text (GTK_ENTRY (g_array_index (widgets->key_entry, GtkWidget *, i)));
        if (!g_str_is_ascii (acc_name)) {
            show_message_dialog (widgets->dialog, "Only ASCII strings are supported", GTK_MESSAGE_ERROR);
            return; // TODO error message
        }
        if (!g_str_is_ascii (acc_key)) {
            show_message_dialog (widgets->dialog, "Only ASCII strings are supported", GTK_MESSAGE_ERROR);
            return; // TODO error message
        }
        g_hash_table_insert (kf_data->data_to_add, g_strdup (acc_name), g_strdup (acc_key));
        i++;
    }
}


static void
cleanup_widgets (Widgets *widgets)
{
    g_array_free (widgets->acc_entry, TRUE);
    g_array_free (widgets->key_entry, TRUE);
    g_free (widgets);
}
