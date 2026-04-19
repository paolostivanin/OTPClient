#include <glib/gi18n.h>
#include "kdf-dialog.h"
#include "gquarks.h"

struct _KdfDialog
{
    AdwDialog parent;

    DatabaseData *db_data;

    GtkWidget *preset_combo;
    GtkWidget *iter_spin;
    GtkWidget *memcost_spin;
    GtkWidget *parallelism_spin;
    GtkWidget *error_label;

    gboolean applying_preset;  /* re-entrancy guard */
};

G_DEFINE_FINAL_TYPE (KdfDialog, kdf_dialog, ADW_TYPE_DIALOG)

/* Index 3 = Custom; the first three rows are honored presets. */
static const struct {
    gint32 iter;
    gint32 memcost;   /* KiB */
    gint32 parallelism;
} kdf_presets[] = {
    { 3, 131072, 2 },   /* Standard  — 128 MiB */
    { 5, 262144, 4 },   /* Strong    — 256 MiB */
    { 8, 524288, 4 },   /* Paranoid  — 512 MiB */
};
#define KDF_PRESET_CUSTOM 3

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

static guint
detect_preset_index (gint32 iter, gint32 memcost, gint32 parallelism)
{
    for (guint i = 0; i < G_N_ELEMENTS (kdf_presets); i++) {
        if (kdf_presets[i].iter == iter &&
            kdf_presets[i].memcost == memcost &&
            kdf_presets[i].parallelism == parallelism)
            return i;
    }
    return KDF_PRESET_CUSTOM;
}

static void
on_preset_changed (AdwComboRow *combo_row,
                    GParamSpec  *pspec,
                    KdfDialog   *self)
{
    (void) pspec;
    guint idx = adw_combo_row_get_selected (combo_row);
    if (idx >= G_N_ELEMENTS (kdf_presets))
        return;  /* Custom — leave spins as-is */

    self->applying_preset = TRUE;
    adw_spin_row_set_value (ADW_SPIN_ROW (self->iter_spin), kdf_presets[idx].iter);
    adw_spin_row_set_value (ADW_SPIN_ROW (self->memcost_spin), kdf_presets[idx].memcost);
    adw_spin_row_set_value (ADW_SPIN_ROW (self->parallelism_spin), kdf_presets[idx].parallelism);
    self->applying_preset = FALSE;
}

static void
on_spin_changed (AdwSpinRow  *spin_row,
                  GParamSpec  *pspec,
                  KdfDialog   *self)
{
    (void) spin_row;
    (void) pspec;
    if (self->applying_preset)
        return;
    /* User edited a spin row — reflect that the values may no longer match a preset. */
    double iter_d = adw_spin_row_get_value (ADW_SPIN_ROW (self->iter_spin));
    double mc_d = adw_spin_row_get_value (ADW_SPIN_ROW (self->memcost_spin));
    double par_d = adw_spin_row_get_value (ADW_SPIN_ROW (self->parallelism_spin));
    guint idx = detect_preset_index ((gint32) iter_d, (gint32) mc_d, (gint32) par_d);
    if (adw_combo_row_get_selected (ADW_COMBO_ROW (self->preset_combo)) != idx) {
        self->applying_preset = TRUE;  /* avoid bouncing back through on_preset_changed */
        adw_combo_row_set_selected (ADW_COMBO_ROW (self->preset_combo), idx);
        self->applying_preset = FALSE;
    }
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

    if (new_iter < 2 || new_mc < 65536 || new_par < 1)
    {
        gtk_label_set_text (GTK_LABEL (self->error_label),
                            _("Invalid parameters: iterations \u2265 2, memory \u2265 64 MiB, parallelism \u2265 1"));
        gtk_widget_set_visible (self->error_label, TRUE);
        return;
    }

    self->db_data->argon2id_iter = new_iter;
    self->db_data->argon2id_memcost = new_mc;
    self->db_data->argon2id_parallelism = new_par;

    /* New KDF parameters invalidate the cached derived key */
    db_invalidate_kdf_cache (self->db_data);

    GError *err = NULL;
    update_db (self->db_data, &err);
    if (err != NULL)
    {
        gtk_label_set_text (GTK_LABEL (self->error_label), err->message);
        gtk_widget_set_visible (self->error_label, TRUE);
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

    /* Preset selector — Standard / Strong / Paranoid / Custom */
    const char * const preset_names[] = {
        N_("Standard (128 MiB)"),
        N_("Strong (256 MiB)"),
        N_("Paranoid (512 MiB)"),
        N_("Custom"),
        NULL
    };
    const char *preset_translated[5];
    for (guint i = 0; preset_names[i] != NULL; i++)
        preset_translated[i] = _(preset_names[i]);
    preset_translated[4] = NULL;
    GtkStringList *preset_model = gtk_string_list_new (preset_translated);
    self->preset_combo = adw_combo_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->preset_combo), _("Preset"));
    adw_combo_row_set_model (ADW_COMBO_ROW (self->preset_combo), G_LIST_MODEL (preset_model));
    gtk_widget_set_tooltip_text (self->preset_combo,
        _("Pick a preset unless you have a reason to tune Argon2id by hand."));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (new_group), self->preset_combo);

    self->iter_spin = adw_spin_row_new_with_range (2, 64, 1);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->iter_spin), _("Iterations"));
    adw_spin_row_set_value (ADW_SPIN_ROW (self->iter_spin), db_data->argon2id_iter);
    gtk_widget_set_tooltip_text (self->iter_spin,
        _("Number of Argon2id passes. Higher values slow brute-force attacks but make unlock take longer."));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (new_group), self->iter_spin);

    self->memcost_spin = adw_spin_row_new_with_range (65536, 1048576, 1024);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->memcost_spin), _("Memory Cost (KiB)"));
    adw_spin_row_set_value (ADW_SPIN_ROW (self->memcost_spin), db_data->argon2id_memcost);
    gtk_widget_set_tooltip_text (self->memcost_spin,
        _("RAM (in KiB) used during key derivation. Higher values defend against GPU and ASIC attacks."));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (new_group), self->memcost_spin);

    self->parallelism_spin = adw_spin_row_new_with_range (1, 16, 1);
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->parallelism_spin), _("Parallelism"));
    adw_spin_row_set_value (ADW_SPIN_ROW (self->parallelism_spin), db_data->argon2id_parallelism);
    gtk_widget_set_tooltip_text (self->parallelism_spin,
        _("Number of threads used in parallel during derivation. Limited by available CPU cores."));
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (new_group), self->parallelism_spin);

    /* Initialize preset selection from current values, then wire signals to avoid
     * spurious cross-talk during construction. */
    guint cur_preset = detect_preset_index (db_data->argon2id_iter,
                                              db_data->argon2id_memcost,
                                              db_data->argon2id_parallelism);
    adw_combo_row_set_selected (ADW_COMBO_ROW (self->preset_combo), cur_preset);

    g_signal_connect (self->preset_combo, "notify::selected", G_CALLBACK (on_preset_changed), self);
    g_signal_connect (self->iter_spin, "notify::value", G_CALLBACK (on_spin_changed), self);
    g_signal_connect (self->memcost_spin, "notify::value", G_CALLBACK (on_spin_changed), self);
    g_signal_connect (self->parallelism_spin, "notify::value", G_CALLBACK (on_spin_changed), self);

    gtk_box_append (GTK_BOX (box), new_group);

    /* Error label */
    self->error_label = gtk_label_new (NULL);
    gtk_widget_add_css_class (self->error_label, "error");
    gtk_widget_set_visible (self->error_label, FALSE);
    gtk_label_set_wrap (GTK_LABEL (self->error_label), TRUE);
    gtk_box_append (GTK_BOX (box), self->error_label);

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
