#include <glib/gi18n.h>
#include "db-info-dialog.h"

struct _DbInfoDialog
{
    AdwDialog parent;
};

G_DEFINE_FINAL_TYPE (DbInfoDialog, db_info_dialog, ADW_TYPE_DIALOG)

static void
db_info_dialog_init (DbInfoDialog *self)
{
    (void) self;
}

static void
db_info_dialog_class_init (DbInfoDialogClass *klass)
{
    (void) klass;
}

static GtkWidget *
make_info_row (const gchar *title,
               const gchar *value)
{
    GtkWidget *row = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), value);
    return row;
}

DbInfoDialog *
db_info_dialog_new (DatabaseData *db_data)
{
    DbInfoDialog *self = g_object_new (DB_INFO_TYPE_DIALOG,
                                        "title", _("Database Info"),
                                        "content-width", 360,
                                        "content-height", -1,
                                        NULL);

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

    GtkWidget *group = adw_preferences_group_new ();

    /* Path */
    gtk_box_append (GTK_BOX (box), make_info_row (_("Path"),
        db_data->db_path ? db_data->db_path : _("Unknown")));

    /* Version */
    g_autofree gchar *version_str = g_strdup_printf ("%d", db_data->current_db_version);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                               make_info_row (_("Database Version"), version_str));

    /* Number of entries */
    gsize entry_count = 0;
    if (db_data->in_memory_json_data != NULL)
        entry_count = json_array_size (db_data->in_memory_json_data);
    g_autofree gchar *count_str = g_strdup_printf ("%zu", entry_count);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                               make_info_row (_("Number of Tokens"), count_str));

    /* KDF parameters */
    GtkWidget *kdf_group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (kdf_group), _("KDF Parameters (Argon2id)"));

    g_autofree gchar *iter_str = g_strdup_printf ("%d", db_data->argon2id_iter);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (kdf_group),
                               make_info_row (_("Iterations"), iter_str));

    g_autofree gchar *mem_str = g_strdup_printf ("%d KiB", db_data->argon2id_memcost);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (kdf_group),
                               make_info_row (_("Memory Cost"), mem_str));

    g_autofree gchar *par_str = g_strdup_printf ("%d", db_data->argon2id_parallelism);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (kdf_group),
                               make_info_row (_("Parallelism"), par_str));

    gtk_box_append (GTK_BOX (box), group);
    gtk_box_append (GTK_BOX (box), kdf_group);

    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), clamp);
    adw_dialog_set_child (ADW_DIALOG (self), toolbar_view);

    return self;
}
