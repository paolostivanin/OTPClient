#include <glib/gi18n.h>
#include <gcrypt.h>
#include "manual-add-dialog.h"
#include "common.h"
#include "db-common.h"

struct _ManualAddDialog
{
    AdwDialog parent;

    DatabaseData *db_data;
    ManualAddCallback callback;
    gpointer callback_data;

    GtkWidget *label_row;
    GtkWidget *issuer_row;
    GtkWidget *secret_row;
    GtkWidget *type_combo;
    GtkWidget *algo_combo;
    GtkWidget *digits_spin;
    GtkWidget *period_spin;
    GtkWidget *counter_spin;
    GtkWidget *period_row;
    GtkWidget *counter_row;
    GtkWidget *add_button;
    GtkWidget *error_label;
};

G_DEFINE_FINAL_TYPE (ManualAddDialog, manual_add_dialog, ADW_TYPE_DIALOG)

static void
update_add_sensitivity (ManualAddDialog *self)
{
    const gchar *label = gtk_editable_get_text (GTK_EDITABLE (self->label_row));
    const gchar *secret = gtk_editable_get_text (GTK_EDITABLE (self->secret_row));

    gboolean sensitive = (label != NULL && label[0] != '\0' &&
                          secret != NULL && secret[0] != '\0');
    gtk_widget_set_sensitive (self->add_button, sensitive);
}

static void
on_field_changed (GtkEditable     *editable,
                  ManualAddDialog *self)
{
    (void) editable;
    update_add_sensitivity (self);
    gtk_widget_set_visible (self->error_label, FALSE);
}

static void
on_type_changed (AdwComboRow     *combo_row,
                 GParamSpec      *pspec,
                 ManualAddDialog *self)
{
    (void) pspec;
    guint selected = adw_combo_row_get_selected (combo_row);

    /* 0 = TOTP, 1 = HOTP */
    gtk_widget_set_visible (self->period_row, selected == 0);
    gtk_widget_set_visible (self->counter_row, selected == 1);
}

static void
on_add_clicked (GtkButton       *button,
                ManualAddDialog *self)
{
    (void) button;

    const gchar *label_text = gtk_editable_get_text (GTK_EDITABLE (self->label_row));
    const gchar *issuer = gtk_editable_get_text (GTK_EDITABLE (self->issuer_row));
    const gchar *secret = gtk_editable_get_text (GTK_EDITABLE (self->secret_row));

    guint type_idx = adw_combo_row_get_selected (ADW_COMBO_ROW (self->type_combo));
    const gchar *type = (type_idx == 0) ? "TOTP" : "HOTP";

    guint algo_idx = adw_combo_row_get_selected (ADW_COMBO_ROW (self->algo_combo));
    const gchar *algo;
    switch (algo_idx)
    {
        case 1: algo = "SHA256"; break;
        case 2: algo = "SHA512"; break;
        default: algo = "SHA1"; break;
    }

    double digits_d = adw_spin_row_get_value (ADW_SPIN_ROW (self->digits_spin));
    guint digits = (guint) digits_d;
    double period_d = adw_spin_row_get_value (ADW_SPIN_ROW (self->period_spin));
    guint period = (guint) period_d;
    double counter_d = adw_spin_row_get_value (ADW_SPIN_ROW (self->counter_spin));
    guint64 counter = (guint64) counter_d;

    json_t *obj = build_json_obj (type, label_text, issuer, secret,
                                   digits, algo, period, counter);

    guint32 hash = json_object_get_hash (obj);
    if (g_slist_find_custom (self->db_data->objects_hash,
                             GUINT_TO_POINTER (hash),
                             check_duplicate) != NULL)
    {
        gtk_label_set_text (GTK_LABEL (self->error_label), _("This token already exists"));
        gtk_widget_set_visible (self->error_label, TRUE);
        json_decref (obj);
        return;
    }

    self->db_data->objects_hash = g_slist_append (self->db_data->objects_hash,
                                                   g_memdup2 (&hash, sizeof (guint32)));
    self->db_data->data_to_add = g_slist_append (self->db_data->data_to_add, obj);

    GError *err = NULL;
    update_db (self->db_data, &err);
    if (err != NULL)
    {
        gtk_label_set_text (GTK_LABEL (self->error_label), err->message);
        gtk_widget_set_visible (self->error_label, TRUE);
        g_clear_error (&err);
        return;
    }

    g_slist_free (self->db_data->data_to_add);
    self->db_data->data_to_add = NULL;

    reload_db (self->db_data, &err);
    if (err != NULL)
    {
        g_warning ("Failed to reload db: %s", err->message);
        g_clear_error (&err);
    }

    adw_dialog_close (ADW_DIALOG (self));

    if (self->callback != NULL)
        self->callback (self->callback_data);
}

static void
manual_add_dialog_dispose (GObject *object)
{
    G_OBJECT_CLASS (manual_add_dialog_parent_class)->dispose (object);
}

static void
manual_add_dialog_init (ManualAddDialog *self)
{
    (void) self;
}

static void
manual_add_dialog_class_init (ManualAddDialogClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = manual_add_dialog_dispose;
}

ManualAddDialog *
manual_add_dialog_new (DatabaseData      *db_data,
                       ManualAddCallback  callback,
                       gpointer           user_data)
{
    ManualAddDialog *self = g_object_new (MANUAL_ADD_TYPE_DIALOG,
                                          "title", _("Add Token"),
                                          "content-width", 400,
                                          "content-height", -1,
                                          NULL);

    self->db_data = db_data;
    self->callback = callback;
    self->callback_data = user_data;

    /* Build UI */
    GtkWidget *toolbar_view = adw_toolbar_view_new ();
    GtkWidget *header = adw_header_bar_new ();
    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);

    GtkWidget *clamp = adw_clamp_new ();
    gtk_widget_set_margin_start (clamp, 12);
    gtk_widget_set_margin_end (clamp, 12);
    gtk_widget_set_margin_top (clamp, 12);
    gtk_widget_set_margin_bottom (clamp, 12);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
    adw_clamp_set_child (ADW_CLAMP (clamp), box);

    /* Account details group */
    GtkWidget *details_group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (details_group), _("Account Details"));

    self->label_row = adw_entry_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->label_row), _("Label"));
    g_signal_connect (self->label_row, "changed", G_CALLBACK (on_field_changed), self);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (details_group), self->label_row);

    self->issuer_row = adw_entry_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->issuer_row), _("Issuer"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (details_group), self->issuer_row);

    self->secret_row = adw_entry_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->secret_row), _("Secret (Base32)"));
    g_signal_connect (self->secret_row, "changed", G_CALLBACK (on_field_changed), self);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (details_group), self->secret_row);

    gtk_box_append (GTK_BOX (box), details_group);

    /* Token settings group */
    GtkWidget *settings_group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (settings_group), _("Token Settings"));

    /* Type dropdown */
    const char * const type_items[] = { "TOTP", "HOTP", NULL };
    GtkStringList *type_model = gtk_string_list_new (type_items);
    self->type_combo = adw_combo_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->type_combo), _("Type"));
    adw_combo_row_set_model (ADW_COMBO_ROW (self->type_combo), G_LIST_MODEL (type_model));
    g_signal_connect (self->type_combo, "notify::selected", G_CALLBACK (on_type_changed), self);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (settings_group), self->type_combo);

    /* Algorithm dropdown */
    const char * const algo_items[] = { "SHA1", "SHA256", "SHA512", NULL };
    GtkStringList *algo_model = gtk_string_list_new (algo_items);
    self->algo_combo = adw_combo_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->algo_combo), _("Algorithm"));
    adw_combo_row_set_model (ADW_COMBO_ROW (self->algo_combo), G_LIST_MODEL (algo_model));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (settings_group), self->algo_combo);

    /* Digits */
    GtkAdjustment *digits_adj = gtk_adjustment_new (6, 4, 10, 1, 1, 0);
    self->digits_spin = adw_spin_row_new (digits_adj, 1, 0);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->digits_spin), _("Digits"));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (settings_group), self->digits_spin);

    /* Period (TOTP) */
    GtkAdjustment *period_adj = gtk_adjustment_new (30, 1, 300, 1, 10, 0);
    self->period_spin = adw_spin_row_new (period_adj, 1, 0);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->period_spin), _("Period (seconds)"));
    self->period_row = self->period_spin;
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (settings_group), self->period_spin);

    /* Counter (HOTP) */
    GtkAdjustment *counter_adj = gtk_adjustment_new (0, 0, G_MAXUINT32, 1, 10, 0);
    self->counter_spin = adw_spin_row_new (counter_adj, 1, 0);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->counter_spin), _("Counter"));
    self->counter_row = self->counter_spin;
    gtk_widget_set_visible (self->counter_row, FALSE);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (settings_group), self->counter_spin);

    gtk_box_append (GTK_BOX (box), settings_group);

    /* Error label */
    self->error_label = gtk_label_new (NULL);
    gtk_widget_add_css_class (self->error_label, "error");
    gtk_widget_set_visible (self->error_label, FALSE);
    gtk_box_append (GTK_BOX (box), self->error_label);

    /* Add button */
    self->add_button = gtk_button_new_with_label (_("Add Token"));
    gtk_widget_add_css_class (self->add_button, "suggested-action");
    gtk_widget_add_css_class (self->add_button, "pill");
    gtk_widget_set_halign (self->add_button, GTK_ALIGN_CENTER);
    gtk_widget_set_sensitive (self->add_button, FALSE);
    g_signal_connect (self->add_button, "clicked", G_CALLBACK (on_add_clicked), self);
    gtk_box_append (GTK_BOX (box), self->add_button);

    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), clamp);
    adw_dialog_set_child (ADW_DIALOG (self), toolbar_view);

    return self;
}
