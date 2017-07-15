#include <gtk/gtk.h>
#include "kf-misc.h"
#include "otpclient.h"

typedef struct _widgets_data {
    gint grid_top;
    gint counter;
} WidgetsData;

typedef struct _widgets {
    GtkWidget *dialog;
    GtkWidget *grid;
    GtkWidget **acc_entry;
    GtkWidget **key_entry;
    WidgetsData *data;
} Widgets;

static gboolean realloc_widgets (Widgets *widgets, gsize num_of_resources_to_realloc);

static void create_header_bar (GtkWidget *dialog);

static void add_entry_cb (GtkWidget *btn, gpointer user_data);

static void add_entry_and_set_icon (Widgets *widgets);

static void update_grid (Widgets *widgets);

static void del_entry_cb (GtkWidget *btn, gpointer user_data);

static void cleanup_widgets (Widgets *widgets);


void
add_data_dialog (GtkWidget *main_win, UpdateData *kf_data)
{
    Widgets *widgets = g_new0 (Widgets, 1);
    widgets->data = g_new0 (WidgetsData, 1);
    widgets->data->counter = 0;
    widgets->data->grid_top = 0;

    widgets->dialog = gtk_dialog_new_with_buttons ("Add Data to Database",
                                                   GTK_WINDOW (main_win),
                                                   GTK_DIALOG_DESTROY_WITH_PARENT, "OK",
                                                   GTK_RESPONSE_OK, "Cancel", GTK_RESPONSE_CANCEL,
                                                   NULL);

    GtkWidget *content_area = gtk_dialog_get_content_area (GTK_DIALOG (widgets->dialog));

    GtkWidget *acc_label = gtk_label_new ("Account Name");
    GtkWidget *key_label = gtk_label_new ("Secret Key");

    if (!realloc_widgets (widgets, 1)) {
        cleanup_widgets (widgets);
        return; //TODO error message
    }

    add_entry_and_set_icon (widgets);

    create_header_bar (widgets->dialog);

    widgets->grid = gtk_grid_new ();
    gtk_container_add (GTK_CONTAINER (content_area), widgets->grid);
    gtk_grid_attach (GTK_GRID (widgets->grid), acc_label, 0, widgets->data->grid_top++, 4, 1);
    gtk_grid_attach_next_to (GTK_GRID (widgets->grid), key_label, acc_label, GTK_POS_RIGHT, 4, 1);
    gtk_grid_attach (GTK_GRID (widgets->grid), widgets->acc_entry[0], 0, widgets->data->grid_top++, 4, 1);
    gtk_grid_attach_next_to (GTK_GRID (widgets->grid), widgets->key_entry[0], widgets->acc_entry[0], GTK_POS_RIGHT, 4, 1);

    gtk_widget_show_all (widgets->dialog);

    gint result = gtk_dialog_run (GTK_DIALOG (widgets->dialog));
    switch (result) {
        case GTK_RESPONSE_CANCEL:
            break;
        default:
            break;
    }
}


static gboolean
realloc_widgets (Widgets *widgets, gsize num_of_resources_to_realloc)
{
    gsize realloc_size = widgets->data->counter + num_of_resources_to_realloc;
    GtkWidget **acc_tmp = g_realloc_n (widgets->acc_entry, realloc_size, sizeof (GtkWidget));
    if (acc_tmp == NULL) {
        return FALSE;
    }
    GtkWidget **key_tmp = g_realloc_n (widgets->key_entry, realloc_size, sizeof (GtkWidget));
    if (key_tmp == NULL) {
        return FALSE;
    }
    widgets->acc_entry = acc_tmp;
    widgets->key_entry = key_tmp;

    widgets->data->counter++;

    return TRUE;
}


static void
create_header_bar (GtkWidget *dialog)
{
    GtkWidget *header = gtk_header_bar_new ();
    gtk_header_bar_set_show_close_button (GTK_HEADER_BAR (header), TRUE);
    gtk_header_bar_set_title (GTK_HEADER_BAR (header), "Add a new account");
    gtk_header_bar_set_has_subtitle (GTK_HEADER_BAR (header), FALSE);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class (gtk_widget_get_style_context (box), "linked");
    GtkWidget *add_button = gtk_button_new ();
    gtk_container_add (GTK_CONTAINER (add_button), gtk_image_new_from_icon_name ("list-add-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_container_add (GTK_CONTAINER (box), add_button);
    GtkWidget *del_button = gtk_button_new ();
    gtk_container_add (GTK_CONTAINER (del_button), gtk_image_new_from_icon_name ("list-remove-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_container_add (GTK_CONTAINER (box), del_button);

    g_signal_connect (add_button, "clicked", G_CALLBACK (add_entry_cb), NULL);
    g_signal_connect (del_button, "clicked", G_CALLBACK (del_entry_cb), NULL);

    gtk_header_bar_pack_start (GTK_HEADER_BAR (header), box);

    gtk_window_set_titlebar (GTK_WINDOW (dialog), header);
}


static void
add_entry_cb (GtkWidget *btn __attribute__((__unused__)),
              gpointer user_data)
{
    Widgets *widgets = (Widgets *)user_data;
    if (!realloc_widgets (widgets, 1)) {
        cleanup_widgets (widgets);
        gtk_widget_destroy (widgets->dialog);
        return; //TODO error message
    }
    add_entry_and_set_icon (widgets);
}


static void
add_entry_and_set_icon (Widgets *widgets)
{
    widgets->acc_entry[widgets->data->counter - 1] = gtk_entry_new ();
    widgets->key_entry[widgets->data->counter - 1] = gtk_entry_new ();
    set_icon_to_entry (widgets->key_entry[widgets->data->counter - 1], "dialog-password-symbolic", "Show password");
    update_grid (widgets);
}


static void
update_grid (Widgets *widgets)
{
    gint current_position = widgets->data->counter - 1;
    gtk_grid_attach (GTK_GRID (widgets->grid), widgets->acc_entry[current_position], 0, widgets->data->grid_top++, 4, 1);
    gtk_grid_attach_next_to (GTK_GRID (widgets->grid), widgets->key_entry[current_position], widgets->acc_entry[current_position], GTK_POS_RIGHT, 4, 1);
}


static void
del_entry_cb (GtkWidget *btn __attribute__((__unused__)),
              gpointer user_data)
{
    Widgets *widgets = (Widgets *)user_data;
    gint current_position = widgets->data->counter - 1;
    gtk_widget_destroy (widgets->key_entry[current_position]);
    gtk_widget_destroy (widgets->acc_entry[current_position]);
    g_free (widgets->key_entry[current_position]);
    g_free (widgets->acc_entry[current_position]);
    widgets->data->counter--;
    widgets->data->grid_top--;
}


static void
cleanup_widgets (Widgets *widgets)
{
    for (gint i = 0; i < widgets->data->counter; i++) {
        g_free (widgets->acc_entry);
        g_free (widgets->key_entry);
    }
    g_free (widgets->data);
    g_free (widgets);
}
