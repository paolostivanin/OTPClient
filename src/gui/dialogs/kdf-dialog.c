#include <glib/gi18n.h>
#include "kdf-dialog.h"
#include "gquarks.h"

struct _KdfDialog
{
    AdwDialog parent;

    DatabaseData *db_data;

    GtkWidget *iter_spin;
    GtkWidget *memcost_spin;
    GtkWidget *parallelism_spin;
};

G_DEFINE_FINAL_TYPE (KdfDialog, kdf_dialog, ADW_TYPE_DIALOG)

static void
kdf_dialog_init (KdfDialog *self)
{
    (void) self;
}

static void
kdf_dialog_class_init (KdfDialogClass *klass)
{
    (void) klass;
}

static void
on_apply_clicked (GtkButton *button,
                  KdfDialog *self)
{
    (void) button;

    double iter_d = adw_spin_row_get_value (ADW_SPIN_ROW (self->iter_spin));
    double mc_d = adw_spin_row_get_value (ADW_SPIN_ROW (self->memcost_spin));
    double par_d = adw_spin_row_get_value (ADW_SPIN_ROW (self->parallelism_spin));

    gint32 new_iter = (gint32) iter_d;
    gint32 new_mc = (gint32) mc_d;
    gint32 new_par = (gint32) par_d;

    if (new_iter < 1 || new_mc < 8192 || new_par < 1)
    {
        g_warning ("Invalid KDF parameters: iter=%d, memcost=%d, parallelism=%d",
                   new_iter, new_mc, new_par);
        return;
    }

    self->db_data->argon2id_iter = new_iter;
    self->db_data->argon2id_memcost = new_mc;
    self->db_data->argon2id_parallelism = new_par;

    GError *err = NULL;
    update_db (self->db_data, &err);
    if (err != NULL)
    {
        g_warning ("Failed to re-encrypt database with new KDF params: %s", err->message);
        g_clear_error (&err);
        return;
    }

    adw_dialog_close (ADW_DIALOG (self));
}

KdfDialog *
kdf_dialog_new (DatabaseData *db_data)
{
    KdfDialog *self = g_object_new (KDF_TYPE_DIALOG,
                                     "title", _("KDF Parameters"),
                                     "content-width", 400,
                                     "content-height", -1,
                                     NULL);

    self->db_data = db_data;

    GtkWidget *toolbar_view = adw_toolbar_view_new ();
    GtkWidget *header = adw_header_bar_new ();
    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);

    GtkWidget *clamp = adw_clamp_new ();
    gtk_widget_set_margin_start (clamp, 12);
    gtk_widget_set_margin_end (clamp, 12);
    gtk_widget_set_margin_top (clamp, 12);
    gtk_widget_set_margin_bottom (clamp, 12);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    adw_clamp_set_child (ADW_CLAMP (clamp), box);

    /* Warning banner */
    GtkWidget *banner = adw_banner_new (_("Changing KDF parameters re-encrypts the database. "
                                           "Use higher values for stronger security (slower unlock)."));
    adw_banner_set_revealed (ADW_BANNER (banner), TRUE);
    gtk_box_append (GTK_BOX (box), banner);

    /* Current values group */
    GtkWidget *cur_group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (cur_group), _("Current Values"));

    g_autofree gchar *cur_iter = g_strdup_printf ("%d", db_data->argon2id_iter);
    g_autofree gchar *cur_mc = g_strdup_printf ("%d KiB", db_data->argon2id_memcost);
    g_autofree gchar *cur_par = g_strdup_printf ("%d", db_data->argon2id_parallelism);

    GtkWidget *r1 = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (r1), _("Iterations"));
    adw_action_row_set_subtitle (ADW_ACTION_ROW (r1), cur_iter);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (cur_group), r1);

    GtkWidget *r2 = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (r2), _("Memory Cost"));
    adw_action_row_set_subtitle (ADW_ACTION_ROW (r2), cur_mc);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (cur_group), r2);

    GtkWidget *r3 = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (r3), _("Parallelism"));
    adw_action_row_set_subtitle (ADW_ACTION_ROW (r3), cur_par);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (cur_group), r3);

    gtk_box_append (GTK_BOX (box), cur_group);

    /* New values group */
    GtkWidget *new_group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (new_group), _("New Values"));

    self->iter_spin = adw_spin_row_new_with_range (1, 64, 1);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->iter_spin), _("Iterations"));
    adw_spin_row_set_value (ADW_SPIN_ROW (self->iter_spin), db_data->argon2id_iter);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (new_group), self->iter_spin);

    self->memcost_spin = adw_spin_row_new_with_range (8192, 1048576, 1024);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->memcost_spin), _("Memory Cost (KiB)"));
    adw_spin_row_set_value (ADW_SPIN_ROW (self->memcost_spin), db_data->argon2id_memcost);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (new_group), self->memcost_spin);

    self->parallelism_spin = adw_spin_row_new_with_range (1, 16, 1);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->parallelism_spin), _("Parallelism"));
    adw_spin_row_set_value (ADW_SPIN_ROW (self->parallelism_spin), db_data->argon2id_parallelism);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (new_group), self->parallelism_spin);

    gtk_box_append (GTK_BOX (box), new_group);

    /* Apply button */
    GtkWidget *apply_btn = gtk_button_new_with_label (_("Apply"));
    gtk_widget_add_css_class (apply_btn, "suggested-action");
    gtk_widget_add_css_class (apply_btn, "pill");
    gtk_widget_set_halign (apply_btn, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top (apply_btn, 12);
    g_signal_connect (apply_btn, "clicked", G_CALLBACK (on_apply_clicked), self);
    gtk_box_append (GTK_BOX (box), apply_btn);

    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), clamp);
    adw_dialog_set_child (ADW_DIALOG (self), toolbar_view);

    return self;
}
