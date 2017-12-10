#include <gtk/gtk.h>
#include "db-misc.h"
#include "otpclient.h"
#include "common.h"
#include "add-data-dialog.h"
#include "gquarks.h"
#include "message-dialogs.h"


static Widgets *init_widgets (void);

static GtkWidget *create_scrolled_window (GtkWidget *content_area);

static void setup_header_bar (Widgets *widgets);

static void add_widgets_cb (GtkWidget *btn, gpointer user_data);

static GtkWidget *get_cb_box (const gchar *id1, const gchar *id2, const gchar *id3,
                              const gchar *text1, const gchar *text2, const gchar *text3,
                              const gchar *active);

static void update_grid (Widgets *widgets);

static void del_entry_cb (GtkWidget *btn, gpointer user_data);

static void sensitive_cb (GtkWidget *cb, gpointer user_data);

static GtkWidget *create_integer_spin_button (void);

static void cleanup_widgets (Widgets *widgets);


int
add_data_dialog (GtkWidget      *main_win,
                 DatabaseData   *db_data,
                 GtkListStore   *list_store)
{
    Widgets *widgets = init_widgets ();

    widgets->dialog = gtk_dialog_new_with_buttons ("Add Data to Database",
                                                   GTK_WINDOW (main_win),
                                                   GTK_DIALOG_DESTROY_WITH_PARENT,
                                                   "OK", GTK_RESPONSE_OK,
                                                   "Cancel", GTK_RESPONSE_CANCEL,
                                                   NULL);

    GtkWidget *content_area = gtk_dialog_get_content_area (GTK_DIALOG (widgets->dialog));

    GtkWidget *type_label = gtk_label_new ("Type");
    GtkWidget *acc_label = gtk_label_new ("Label");
    GtkWidget *iss_label = gtk_label_new ("Issuer");
    GtkWidget *key_label = gtk_label_new ("Secret");
    GtkWidget *dig_label = gtk_label_new ("Digits");
    GtkWidget *alg_label = gtk_label_new ("Algo");
    GtkWidget *ctr_label = gtk_label_new ("Counter");

    GtkWidget *scrolled_win = create_scrolled_window (content_area);

    setup_header_bar (widgets);

    widgets->grid = gtk_grid_new ();
    gtk_grid_set_column_spacing (GTK_GRID (widgets->grid), 3);
    gtk_grid_set_row_spacing (GTK_GRID (widgets->grid), 3);
    gtk_container_add (GTK_CONTAINER (scrolled_win), widgets->grid);
    gtk_grid_attach (GTK_GRID (widgets->grid), type_label, 0, widgets->grid_top++, 2, 1);
    gtk_grid_attach_next_to (GTK_GRID (widgets->grid), acc_label, type_label, GTK_POS_RIGHT, 4, 1);
    gtk_grid_attach_next_to (GTK_GRID (widgets->grid), iss_label, acc_label, GTK_POS_RIGHT, 4, 1);
    gtk_grid_attach_next_to (GTK_GRID (widgets->grid), key_label, iss_label, GTK_POS_RIGHT, 4, 1);
    gtk_grid_attach_next_to (GTK_GRID (widgets->grid), dig_label, key_label, GTK_POS_RIGHT, 2, 1);
    gtk_grid_attach_next_to (GTK_GRID (widgets->grid), alg_label, dig_label, GTK_POS_RIGHT, 2, 1);
    gtk_grid_attach_next_to (GTK_GRID (widgets->grid), ctr_label, alg_label, GTK_POS_RIGHT, 2, 1);

    add_widgets_cb (NULL, widgets);

    GError *err = NULL;
    gint result = gtk_dialog_run (GTK_DIALOG (widgets->dialog));
    switch (result) {
        case GTK_RESPONSE_OK:
            if (parse_user_data (widgets, db_data)) {
                update_and_reload_db (db_data, list_store, TRUE, &err);
                if (err != NULL && !g_error_matches (err, missing_file_gquark (), MISSING_FILE_CODE)) {
                    show_message_dialog (main_win, err->message, GTK_MESSAGE_ERROR);
                }
            }
            break;
        case GTK_RESPONSE_CANCEL:
        default:
            break;
    }
    gtk_widget_destroy (widgets->dialog);
    cleanup_widgets (widgets);

    return 0;
}


static Widgets *
init_widgets ()
{
    Widgets *w = g_new0 (Widgets, 1);
    w->type_cb_box = g_array_new (FALSE, FALSE, sizeof (GtkWidget *));
    w->acc_entry = g_array_new (FALSE, FALSE, sizeof (GtkWidget *));
    w->iss_entry = g_array_new (FALSE, FALSE, sizeof (GtkWidget *));
    w->key_entry = g_array_new (FALSE, FALSE, sizeof (GtkWidget *));
    w->dig_cb_box = g_array_new (FALSE, FALSE, sizeof (GtkWidget *));
    w->alg_cb_box = g_array_new (FALSE, FALSE, sizeof (GtkWidget *));
    w->spin_btn = g_array_new (FALSE, FALSE, sizeof (GtkWidget *));

    w->grid_top = 0;

    return w;
}


static GtkWidget *
create_scrolled_window (GtkWidget *content_area)
{
    GtkWidget *sw = gtk_scrolled_window_new (NULL, NULL);
    gtk_scrolled_window_set_shadow_type (GTK_SCROLLED_WINDOW (sw), GTK_SHADOW_ETCHED_IN);
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width (GTK_SCROLLED_WINDOW (sw), TRUE);
    gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (sw), 200);
    gtk_container_add (GTK_CONTAINER (content_area), sw);

    g_object_set (sw, "expand", TRUE, NULL);

    return sw;
}


static void
setup_header_bar (Widgets *widgets)
{
    GtkWidget *header_bar = create_header_bar ("Add New Account(s)");
    GtkWidget *box = create_box_with_buttons ("add_btn_dialog", "del_btn_dialog", NULL);
    gtk_header_bar_pack_start (GTK_HEADER_BAR (header_bar), box);
    gtk_window_set_titlebar (GTK_WINDOW (widgets->dialog), header_bar);
    g_signal_connect (find_widget (box, "add_btn_dialog"), "clicked", G_CALLBACK (add_widgets_cb), widgets);
    g_signal_connect (find_widget (box, "del_btn_dialog"), "clicked", G_CALLBACK (del_entry_cb), widgets);
}


static void
add_widgets_cb (GtkWidget *btn __attribute__((__unused__)),
              gpointer   user_data)
{
    Widgets *widgets = (Widgets *) user_data;

    GtkWidget *acc_entry = gtk_entry_new ();
    GtkWidget *iss_entry = gtk_entry_new ();
    GtkWidget *key_entry = gtk_entry_new ();

    GtkWidget *type_cb_box = get_cb_box ("totp", "hotp", NULL, "TOTP", "HOTP", NULL, "totp");
    GtkWidget *dig_cb_box = get_cb_box ("6digits", "8digits", NULL, "6", "8", NULL, "6digits");
    GtkWidget *alg_cb_box = get_cb_box ("sha1", "sha256", "sha512", "SHA1", "SHA256", "SHA512", "sha1");

    GtkWidget *sb = create_integer_spin_button ();
    g_signal_connect (type_cb_box, "changed", G_CALLBACK (sensitive_cb), sb);

    g_array_append_val (widgets->type_cb_box, type_cb_box);
    g_array_append_val (widgets->acc_entry, acc_entry);
    g_array_append_val (widgets->iss_entry, iss_entry);
    g_array_append_val (widgets->key_entry, key_entry);
    g_array_append_val (widgets->dig_cb_box, dig_cb_box);
    g_array_append_val (widgets->alg_cb_box, alg_cb_box);
    g_array_append_val (widgets->spin_btn, sb);
    set_icon_to_entry (g_array_index (widgets->key_entry, GtkWidget *, widgets->key_entry->len - 1),
                       "dialog-password-symbolic",
                       "Show password");

    update_grid (widgets);
}


static GtkWidget *
get_cb_box (const gchar *id1,   const gchar *id2,   const gchar *id3,
            const gchar *text1, const gchar *text2, const gchar *text3,
            const gchar *active)
{
    GtkWidget *cb = gtk_combo_box_text_new ();
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (cb), id1, text1);
    gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (cb), id2, text2);
    if (id3 != NULL) {
        gtk_combo_box_text_append (GTK_COMBO_BOX_TEXT (cb), id3, text3);
    }
    gtk_combo_box_set_active_id (GTK_COMBO_BOX (cb), active);

    return cb;
}


static void
update_grid (Widgets *widgets)
{
    GtkWidget *type_cb_to_add = g_array_index (widgets->type_cb_box, GtkWidget *, widgets->type_cb_box->len - 1);
    GtkWidget *acc_entry_to_add = g_array_index (widgets->acc_entry, GtkWidget *, widgets->acc_entry->len - 1);
    gtk_widget_set_hexpand (acc_entry_to_add, TRUE);
    GtkWidget *iss_entry_to_add = g_array_index (widgets->iss_entry, GtkWidget *, widgets->iss_entry->len - 1);
    gtk_widget_set_hexpand (iss_entry_to_add, TRUE);
    GtkWidget *key_entry_to_add = g_array_index (widgets->key_entry, GtkWidget *, widgets->key_entry->len - 1);
    gtk_widget_set_hexpand (key_entry_to_add, TRUE);
    GtkWidget *dig_cb_to_add = g_array_index (widgets->dig_cb_box, GtkWidget *, widgets->dig_cb_box->len - 1);
    GtkWidget *alg_cb_to_add = g_array_index (widgets->alg_cb_box, GtkWidget *, widgets->alg_cb_box->len - 1);
    GtkWidget *spin_to_add = g_array_index (widgets->spin_btn, GtkWidget *, widgets->spin_btn->len - 1);

    gtk_grid_attach (GTK_GRID (widgets->grid), type_cb_to_add, 0, widgets->grid_top++, 2, 1);
    gtk_grid_attach_next_to (GTK_GRID (widgets->grid), acc_entry_to_add, type_cb_to_add, GTK_POS_RIGHT, 4, 1);
    gtk_grid_attach_next_to (GTK_GRID (widgets->grid), iss_entry_to_add, acc_entry_to_add, GTK_POS_RIGHT, 4, 1);
    gtk_grid_attach_next_to (GTK_GRID (widgets->grid), key_entry_to_add, iss_entry_to_add, GTK_POS_RIGHT, 4, 1);
    gtk_grid_attach_next_to (GTK_GRID (widgets->grid), dig_cb_to_add, key_entry_to_add, GTK_POS_RIGHT, 2, 1);
    gtk_grid_attach_next_to (GTK_GRID (widgets->grid), alg_cb_to_add, dig_cb_to_add, GTK_POS_RIGHT, 2, 1);
    gtk_grid_attach_next_to (GTK_GRID (widgets->grid), spin_to_add, alg_cb_to_add, GTK_POS_RIGHT, 2, 1);

    gtk_widget_show_all (widgets->dialog);
}


static void
del_entry_cb (GtkWidget *btn __attribute__((__unused__)),
              gpointer   user_data)
{
    Widgets *widgets = (Widgets *) user_data;
    guint current_position = widgets->acc_entry->len - 1;
    if (current_position == 0) //at least one row has to remain
        return;

    gtk_grid_remove_row (GTK_GRID (widgets->grid), --widgets->grid_top);

    g_array_remove_index (widgets->type_cb_box, current_position);
    g_array_remove_index (widgets->acc_entry, current_position);
    g_array_remove_index (widgets->iss_entry, current_position);
    g_array_remove_index (widgets->key_entry, current_position);
    g_array_remove_index (widgets->dig_cb_box, current_position);
    g_array_remove_index (widgets->alg_cb_box, current_position);
    g_array_remove_index (widgets->spin_btn, current_position);
}


static void
sensitive_cb (GtkWidget *cb,
              gpointer   user_data)
{
    GtkWidget *sb = (GtkWidget *) user_data;
    if (g_strcmp0 (gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (cb)), "HOTP") == 0) {
        gtk_widget_set_sensitive (sb, TRUE);
    } else {
        gtk_widget_set_sensitive (sb, FALSE);
    }
}


static GtkWidget *
create_integer_spin_button ()
{
    GtkAdjustment *adjustment = gtk_adjustment_new (0.0, 0.0, G_MAXUINT32, 1.0, 5.0, 0.0);
    GtkWidget *sb = gtk_spin_button_new (adjustment, 1.0, 0);
    gtk_widget_set_sensitive (sb, FALSE);

    return sb;
}


static void
cleanup_widgets (Widgets *widgets)
{
    g_array_free (widgets->type_cb_box, TRUE);
    g_array_free (widgets->acc_entry, TRUE);
    g_array_free (widgets->iss_entry, TRUE);
    g_array_free (widgets->key_entry, TRUE);
    g_array_free (widgets->dig_cb_box, TRUE);
    g_array_free (widgets->alg_cb_box, TRUE);
    g_array_free (widgets->spin_btn, TRUE);
    g_free (widgets);
}
