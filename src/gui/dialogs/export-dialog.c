#include <glib/gi18n.h>
#include "export-dialog.h"
#include "import-export.h"

struct _ExportDialog
{
    AdwDialog parent;

    DatabaseData *db_data;
    GtkWidget *parent_widget;

    GtkWidget *format_combo;
    GtkWidget *password_row;
    GtkWidget *password_confirm_row;
    GtkWidget *export_button;
    GtkWidget *error_label;
};

typedef enum
{
    EXPORT_FMT_FREEOTPPLUS,
    EXPORT_FMT_AEGIS_PLAIN,
    EXPORT_FMT_AEGIS_ENC,
    EXPORT_FMT_AUTHPRO_PLAIN,
    EXPORT_FMT_AUTHPRO_ENC,
    EXPORT_FMT_TWOFAS_PLAIN,
    EXPORT_FMT_TWOFAS_ENC,
    N_EXPORT_FORMATS
} ExportFormat;

G_DEFINE_FINAL_TYPE (ExportDialog, export_dialog, ADW_TYPE_DIALOG)

static void
on_format_changed (AdwComboRow  *combo_row,
                   GParamSpec   *pspec,
                   ExportDialog *self)
{
    (void) pspec;
    guint selected = adw_combo_row_get_selected (combo_row);
    gboolean needs_password = (selected == EXPORT_FMT_AEGIS_ENC ||
                                selected == EXPORT_FMT_AUTHPRO_ENC ||
                                selected == EXPORT_FMT_TWOFAS_ENC);
    gtk_widget_set_visible (self->password_row, needs_password);
    gtk_widget_set_visible (self->password_confirm_row, needs_password);
}

static void
on_file_dialog_save_complete (GObject      *source,
                              GAsyncResult *result,
                              gpointer      user_data)
{
    ExportDialog *self = EXPORT_DIALOG (user_data);
    GtkFileDialog *dialog = GTK_FILE_DIALOG (source);

    GError *err = NULL;
    GFile *file = gtk_file_dialog_save_finish (dialog, result, &err);
    if (file == NULL)
    {
        g_clear_error (&err);
        return;
    }

    g_autofree gchar *path = g_file_get_path (file);
    g_object_unref (file);

    guint fmt = adw_combo_row_get_selected (ADW_COMBO_ROW (self->format_combo));
    const gchar *password = NULL;
    if (gtk_widget_get_visible (self->password_row))
    {
        password = gtk_editable_get_text (GTK_EDITABLE (self->password_row));
        const gchar *confirm = gtk_editable_get_text (GTK_EDITABLE (self->password_confirm_row));
        if (g_strcmp0 (password, confirm) != 0)
        {
            gtk_label_set_text (GTK_LABEL (self->error_label), _("Passwords do not match"));
            gtk_widget_set_visible (self->error_label, TRUE);
            return;
        }
    }

    gchar *error_msg = NULL;
    switch (fmt)
    {
        case EXPORT_FMT_FREEOTPPLUS:
            error_msg = export_freeotpplus (path, self->db_data->in_memory_json_data);
            break;
        case EXPORT_FMT_AEGIS_PLAIN:
            error_msg = export_aegis (path, NULL, self->db_data->in_memory_json_data);
            break;
        case EXPORT_FMT_AEGIS_ENC:
            error_msg = export_aegis (path, password, self->db_data->in_memory_json_data);
            break;
        case EXPORT_FMT_AUTHPRO_PLAIN:
            error_msg = export_authpro (path, NULL, self->db_data->in_memory_json_data);
            break;
        case EXPORT_FMT_AUTHPRO_ENC:
            error_msg = export_authpro (path, password, self->db_data->in_memory_json_data);
            break;
        case EXPORT_FMT_TWOFAS_PLAIN:
            error_msg = export_twofas (path, NULL, self->db_data->in_memory_json_data);
            break;
        case EXPORT_FMT_TWOFAS_ENC:
            error_msg = export_twofas (path, password, self->db_data->in_memory_json_data);
            break;
        default:
            break;
    }

    /* Clear password entry widgets after use */
    if (gtk_widget_get_visible (self->password_row))
    {
        gtk_editable_set_text (GTK_EDITABLE (self->password_row), "");
        gtk_editable_set_text (GTK_EDITABLE (self->password_confirm_row), "");
    }

    if (error_msg != NULL)
    {
        gtk_label_set_text (GTK_LABEL (self->error_label), error_msg);
        gtk_widget_set_visible (self->error_label, TRUE);
        g_free (error_msg);
        return;
    }

    adw_dialog_close (ADW_DIALOG (self));
}

static void
on_export_clicked (GtkButton    *button,
                   ExportDialog *self)
{
    (void) button;

    GtkFileDialog *dialog = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dialog, _("Export tokens"));

    GtkWindow *win = GTK_WINDOW (gtk_widget_get_root (self->parent_widget));
    gtk_file_dialog_save (dialog, win, NULL,
                          on_file_dialog_save_complete, self);
}

static void
export_dialog_init (ExportDialog *self)
{
    (void) self;
}

static void
export_dialog_class_init (ExportDialogClass *klass)
{
    (void) klass;
}

ExportDialog *
export_dialog_new (DatabaseData *db_data,
                   GtkWidget    *parent)
{
    ExportDialog *self = g_object_new (EXPORT_TYPE_DIALOG,
                                       "title", _("Export Tokens"),
                                       "content-width", 360,
                                       "content-height", -1,
                                       NULL);

    self->db_data = db_data;
    self->parent_widget = parent;

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

    GtkWidget *group = adw_preferences_group_new ();

    const char * const format_items[] = {
        "FreeOTP+ (Plain)",
        "Aegis (Plain JSON)",
        "Aegis (Encrypted)",
        "AuthPro (Plain JSON)",
        "AuthPro (Encrypted)",
        "2FAS (Plain JSON)",
        "2FAS (Encrypted)",
        NULL
    };

    GtkStringList *format_model = gtk_string_list_new (format_items);
    self->format_combo = adw_combo_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->format_combo), _("Format"));
    adw_combo_row_set_model (ADW_COMBO_ROW (self->format_combo), G_LIST_MODEL (format_model));
    g_signal_connect (self->format_combo, "notify::selected", G_CALLBACK (on_format_changed), self);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), self->format_combo);

    /* Password for encrypted formats */
    self->password_row = adw_password_entry_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->password_row), _("Encryption Password"));
    gtk_widget_set_visible (self->password_row, FALSE);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), self->password_row);

    self->password_confirm_row = adw_password_entry_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->password_confirm_row), _("Confirm Password"));
    gtk_widget_set_visible (self->password_confirm_row, FALSE);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), self->password_confirm_row);

    gtk_box_append (GTK_BOX (box), group);

    /* Warning banner */
    GtkWidget *warning = adw_banner_new (_("Exporting unencrypted reveals all secrets!"));
    adw_banner_set_revealed (ADW_BANNER (warning), TRUE);
    gtk_box_append (GTK_BOX (box), warning);

    /* Error label */
    self->error_label = gtk_label_new (NULL);
    gtk_widget_add_css_class (self->error_label, "error");
    gtk_widget_set_visible (self->error_label, FALSE);
    gtk_box_append (GTK_BOX (box), self->error_label);

    /* Export button */
    self->export_button = gtk_button_new_with_label (_("Export"));
    gtk_widget_add_css_class (self->export_button, "suggested-action");
    gtk_widget_add_css_class (self->export_button, "pill");
    gtk_widget_set_halign (self->export_button, GTK_ALIGN_CENTER);
    g_signal_connect (self->export_button, "clicked", G_CALLBACK (on_export_clicked), self);
    gtk_box_append (GTK_BOX (box), self->export_button);

    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), clamp);
    adw_dialog_set_child (ADW_DIALOG (self), toolbar_view);

    return self;
}
