#define _DEFAULT_SOURCE
#include "sensitive-dialog.h"
#include <glib/gi18n.h>
#include <string.h>
#include "export-dialog.h"
#include "import-export.h"
#include "../otpclient-application.h"

struct _ExportDialog
{
    AdwDialog parent;

    DatabaseData *db_data;
    json_t *tokens_override;
    GtkWidget *parent_widget;

    GtkWidget *format_combo;
    GtkWidget *password_row;
    GtkWidget *password_confirm_row;
    GtkWidget *export_button;
    GtkWidget *error_label;
    GtkWidget *plain_warning;
    gboolean disposed;
    gboolean export_running;
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

typedef struct {
    gchar *path;
    guint format;
    json_t *tokens;
    gchar *password;
} ExportTaskData;

typedef struct {
    gchar *error;
} ExportTaskResult;

static void
wipe_token_array (json_t *tokens)
{
    if (!json_is_array (tokens))
        return;
    size_t i;
    json_t *token;
    json_array_foreach (tokens, i, token) {
        json_t *secret = json_object_get (token, "secret");
        if (json_is_string (secret)) {
            const char *value = json_string_value (secret);
            size_t len = json_string_length (secret);
            if (value != NULL && len > 0)
                explicit_bzero ((void *) value, len);
        }
    }
}

static void
export_task_data_free (gpointer data)
{
    ExportTaskData *td = data;
    g_free (td->path);
    wipe_token_array (td->tokens);
    g_clear_pointer (&td->tokens, json_decref);
    if (td->password != NULL) {
        explicit_bzero (td->password, strlen (td->password));
        gcry_free (td->password);
    }
    g_free (td);
}

static void
export_task_result_free (gpointer data)
{
    ExportTaskResult *result = data;
    g_free (result->error);
    g_free (result);
}

G_DEFINE_FINAL_TYPE (ExportDialog, export_dialog, ADW_TYPE_DIALOG)

static void
wipe_password_row (GtkWidget *row)
{
    if (row == NULL)
        return;
    const gchar *text = gtk_editable_get_text (GTK_EDITABLE (row));
    if (text != NULL && text[0] != '\0')
        explicit_bzero ((gchar *) text, strlen (text));
    gtk_editable_set_text (GTK_EDITABLE (row), "");
}

/* Both rows at once, for the paths that end the export for good. Skipped once
 * dispose has run, since the template children are gone by then and dispose
 * has already wiped them. */
static void
wipe_password_rows (ExportDialog *self)
{
    if (self->disposed)
        return;
    wipe_password_row (self->password_row);
    wipe_password_row (self->password_confirm_row);
}

static void
clear_selected_tokens (AdwDialog *dialog)
{
    ExportDialog *self = EXPORT_DIALOG (dialog);
    if (self->tokens_override == NULL)
        return;
    wipe_token_array (self->tokens_override);
    json_decref (self->tokens_override);
    self->tokens_override = NULL;
}

static gboolean
export_password_valid (ExportDialog *self)
{
    if (!gtk_widget_get_visible (self->password_row))
        return TRUE;
    const gchar *password = gtk_editable_get_text (GTK_EDITABLE (self->password_row));
    const gchar *confirm = gtk_editable_get_text (GTK_EDITABLE (self->password_confirm_row));
    return password[0] != '\0' && g_strcmp0 (password, confirm) == 0;
}

static void
on_password_changed (GtkEditable *editable, ExportDialog *self)
{
    (void) editable;
    gtk_widget_set_sensitive (self->export_button, export_password_valid (self));
}

static void
on_format_changed (AdwComboRow  *combo_row,
                   GParamSpec   *pspec,
                   ExportDialog *self)
{
    (void) pspec;
    guint selected = adw_combo_row_get_selected (combo_row);
    gboolean is_plain = (selected == EXPORT_FMT_FREEOTPPLUS ||
                         selected == EXPORT_FMT_AEGIS_PLAIN ||
                         selected == EXPORT_FMT_AUTHPRO_PLAIN ||
                         selected == EXPORT_FMT_TWOFAS_PLAIN);
    gboolean needs_password = (selected == EXPORT_FMT_AEGIS_ENC ||
                                selected == EXPORT_FMT_AUTHPRO_ENC ||
                                selected == EXPORT_FMT_TWOFAS_ENC);
    gtk_widget_set_visible (self->password_row, needs_password);
    gtk_widget_set_visible (self->password_confirm_row, needs_password);
    adw_banner_set_revealed (ADW_BANNER (self->plain_warning), is_plain);
    on_password_changed (NULL, self);
}

static void
export_tokens_thread (GTask *task, gpointer source, gpointer task_data,
                      GCancellable *cancellable)
{
    (void) source;
    ExportTaskData *td = task_data;
    if (g_cancellable_is_cancelled (cancellable)) {
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                 "Export cancelled");
        return;
    }

    gchar *error = NULL;
    switch (td->format) {
        case EXPORT_FMT_FREEOTPPLUS:
            error = export_freeotpplus (td->path, td->tokens);
            break;
        case EXPORT_FMT_AEGIS_PLAIN:
            error = export_aegis (td->path, NULL, td->tokens);
            break;
        case EXPORT_FMT_AEGIS_ENC:
            error = export_aegis (td->path, td->password, td->tokens);
            break;
        case EXPORT_FMT_AUTHPRO_PLAIN:
            error = export_authpro (td->path, NULL, td->tokens);
            break;
        case EXPORT_FMT_AUTHPRO_ENC:
            error = export_authpro (td->path, td->password, td->tokens);
            break;
        case EXPORT_FMT_TWOFAS_PLAIN:
            error = export_twofas (td->path, NULL, td->tokens);
            break;
        case EXPORT_FMT_TWOFAS_ENC:
            error = export_twofas (td->path, td->password, td->tokens);
            break;
        default:
            error = g_strdup (_("Unsupported export format."));
            break;
    }
    if (g_cancellable_is_cancelled (cancellable)) {
        g_free (error);
        g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                 "Export cancelled");
        return;
    }
    ExportTaskResult *result = g_new0 (ExportTaskResult, 1);
    result->error = error;
    g_task_return_pointer (task, result, export_task_result_free);
}

static void
export_tokens_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void) source;
    /* The task holds no reference to the dialog, so its last unref on the
     * worker cannot dispose GTK widgets there. This callback runs on the main
     * thread and drops the reference it was given. */
    g_autoptr (ExportDialog) self = user_data;
    self->export_running = FALSE;
    gtk_widget_set_sensitive (self->format_combo, TRUE);
    gtk_widget_set_sensitive (self->password_row, TRUE);
    gtk_widget_set_sensitive (self->password_confirm_row, TRUE);
    gtk_button_set_label (GTK_BUTTON (self->export_button), _("Export"));
    gtk_widget_set_sensitive (self->export_button, TRUE);
    if (sensitive_dialog_is_closed (ADW_DIALOG (self)))
        return;

    GError *err = NULL;
    ExportTaskResult *export_result = g_task_propagate_pointer (G_TASK (result), &err);
    if (err != NULL) {
        if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            gtk_label_set_text (GTK_LABEL (self->error_label), err->message);
            gtk_widget_set_visible (self->error_label, TRUE);
        }
        g_clear_error (&err);
        return;
    }
    if (export_result->error != NULL) {
        gtk_label_set_text (GTK_LABEL (self->error_label), export_result->error);
        gtk_widget_add_css_class (self->error_label, "error");
        gtk_widget_remove_css_class (self->error_label, "success");
    } else {
        gtk_label_set_text (GTK_LABEL (self->error_label),
                            _("Export completed successfully."));
        gtk_widget_remove_css_class (self->error_label, "error");
        gtk_widget_add_css_class (self->error_label, "success");
        wipe_password_rows (self);
    }
    gtk_widget_set_visible (self->error_label, TRUE);
    export_task_result_free (export_result);
}

static void
on_file_dialog_save_complete (GObject      *source,
                              GAsyncResult *result,
                              gpointer      user_data)
{
    g_autoptr (ExportDialog) self = EXPORT_DIALOG (user_data);
    GtkFileDialog *dialog = GTK_FILE_DIALOG (source);

    GError *err = NULL;
    GFile *file = gtk_file_dialog_save_finish (dialog, result, &err);
    if (sensitive_dialog_is_closed (ADW_DIALOG (self))) {
        g_clear_object (&file);
        g_clear_error (&err);
        wipe_password_rows (self);
        return;
    }
    /* Cancelled the file chooser: the dialog is still up and the user may well
     * pick another destination, so the typed password stays where it is. */
    if (file == NULL)
    {
        g_clear_error (&err);
        return;
    }

    GApplication *default_app = g_application_get_default ();
    OTPClientApplication *app = OTPCLIENT_IS_APPLICATION (default_app)
        ? OTPCLIENT_APPLICATION (default_app) : NULL;
    if (app == NULL || otpclient_application_get_app_locked (app) ||
        otpclient_application_is_database_operation_in_progress (app) ||
        otpclient_application_get_db_data (app) != self->db_data)
    {
        g_object_unref (file);
        wipe_password_rows (self);
        return;
    }
    g_autofree gchar *path = g_file_get_path (file);
    g_object_unref (file);
    /* g_file_get_path answers NULL for a non-native location (sftp, smb, MTP,
     * Drive), which the chooser offers freely. Every exporter below would hand
     * that NULL to g_file_new_for_path and report success over a file nobody
     * wrote, so the user would be told their backup exists when it does not. */
    if (path == NULL)
    {
        gtk_label_set_text (GTK_LABEL (self->error_label),
                            _("That location is not a file on this computer. Pick a local folder instead."));
        gtk_widget_set_visible (self->error_label, TRUE);
        return;
    }

    guint fmt = adw_combo_row_get_selected (ADW_COMBO_ROW (self->format_combo));
    const gchar *password = NULL;
    if (gtk_widget_get_visible (self->password_row))
    {
        password = gtk_editable_get_text (GTK_EDITABLE (self->password_row));
        const gchar *confirm = gtk_editable_get_text (GTK_EDITABLE (self->password_confirm_row));
        if (password[0] == '\0' || g_strcmp0 (password, confirm) != 0)
        {
            gtk_label_set_text (GTK_LABEL (self->error_label), _("Passwords do not match"));
            gtk_widget_set_visible (self->error_label, TRUE);
            return;
        }
    }

    json_t *export_tokens = self->tokens_override != NULL
        ? self->tokens_override : self->db_data->in_memory_json_data;
    ExportTaskData *td = g_new0 (ExportTaskData, 1);
    td->path = g_strdup (path);
    td->format = fmt;
    td->tokens = json_deep_copy (export_tokens);
    if (password != NULL) {
        gsize len = strlen (password);
        td->password = gcry_calloc_secure (len + 1, 1);
        if (td->password != NULL)
            memcpy (td->password, password, len);
    }
    if (td->tokens == NULL || (password != NULL && td->password == NULL)) {
        export_task_data_free (td);
        gtk_label_set_text (GTK_LABEL (self->error_label),
                            _("Could not prepare the export in secure memory."));
        gtk_widget_set_visible (self->error_label, TRUE);
        wipe_password_rows (self);
        return;
    }

    self->export_running = TRUE;
    gtk_widget_set_sensitive (self->format_combo, FALSE);
    gtk_widget_set_sensitive (self->password_row, FALSE);
    gtk_widget_set_sensitive (self->password_confirm_row, FALSE);
    gtk_button_set_label (GTK_BUTTON (self->export_button), _("Exporting…"));
    gtk_widget_set_sensitive (self->export_button, FALSE);
    gtk_widget_set_visible (self->error_label, FALSE);
    wipe_password_rows (self);
    GTask *task = g_task_new (NULL, sensitive_dialog_get_cancellable (ADW_DIALOG (self)),
                              export_tokens_done, g_object_ref (self));
    g_task_set_task_data (task, td, export_task_data_free);
    g_task_run_in_thread (task, export_tokens_thread);
    g_object_unref (task);
}

static void
on_export_clicked (GtkButton    *button,
                   ExportDialog *self)
{
    (void) button;

    if (sensitive_dialog_is_closed (ADW_DIALOG (self)) || self->export_running ||
        !export_password_valid (self))
        return;

    GtkFileDialog *dialog = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dialog, _("Export tokens"));
    static const char * const export_names[] = {
        "otpclient-freeotp.txt",
        "otpclient-aegis.json",
        "otpclient-aegis.json",
        "otpclient-authpro.json",
        "otpclient-authpro.bin",
        "otpclient-2fas.json",
        "otpclient-2fas.json"
    };
    guint fmt = adw_combo_row_get_selected (ADW_COMBO_ROW (self->format_combo));
    gtk_file_dialog_set_initial_name (dialog,
                                     export_names[MIN (fmt, N_EXPORT_FORMATS - 1)]);

    GtkWindow *win = GTK_WINDOW (gtk_widget_get_root (self->parent_widget));
    gtk_file_dialog_save (dialog, win, sensitive_dialog_get_cancellable (ADW_DIALOG (self)),
                          on_file_dialog_save_complete, g_object_ref (self));
    g_object_unref (dialog);
}

static void
export_dialog_dispose (GObject *object)
{
    ExportDialog *self = EXPORT_DIALOG (object);
    if (!self->disposed) {
        self->disposed = TRUE;
        wipe_password_row (self->password_row);
        wipe_password_row (self->password_confirm_row);
        g_clear_pointer (&self->db_data, database_data_free);
        clear_selected_tokens (ADW_DIALOG (self));
    }

    G_OBJECT_CLASS (export_dialog_parent_class)->dispose (object);
}

static void
export_dialog_init (ExportDialog *self)
{
    (void) self;
}

static void
export_dialog_class_init (ExportDialogClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = export_dialog_dispose;
}

ExportDialog *
export_dialog_new_selected (DatabaseData *db_data,
                            GtkWidget    *parent,
                            json_t       *tokens)
{
    g_return_val_if_fail (json_is_array (tokens), NULL);
    ExportDialog *self = g_object_new (EXPORT_TYPE_DIALOG,
                                       "title", _("Export Selected Tokens"),
                                       "content-width", 440,
                                       "content-height", 440,
                                       NULL);

    self->db_data = database_data_ref (db_data);
    self->tokens_override = json_deep_copy (tokens);
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
        N_("FreeOTP+ (Plain)"),
        N_("Aegis (Plain JSON)"),
        N_("Aegis (Encrypted)"),
        N_("AuthPro (Plain JSON)"),
        N_("AuthPro (Encrypted)"),
        N_("2FAS (Plain JSON)"),
        N_("2FAS (Encrypted)"),
        NULL
    };

    const char *translated_items[N_EXPORT_FORMATS + 1];
    for (guint i = 0; i < N_EXPORT_FORMATS; i++)
        translated_items[i] = _(format_items[i]);
    translated_items[N_EXPORT_FORMATS] = NULL;
    GtkStringList *format_model = gtk_string_list_new (translated_items);
    self->format_combo = adw_combo_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->format_combo), _("Format"));
    adw_combo_row_set_model (ADW_COMBO_ROW (self->format_combo), G_LIST_MODEL (format_model));
    g_object_unref (format_model);
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

    /* Warning banner - revealed for the four plain formats and hidden for the
     * three encrypted ones (see on_format_changed). The default selection is
     * FreeOTP+ (Plain), which is plaintext, so we start with it visible. */
    self->plain_warning = adw_banner_new (_("Exporting unencrypted reveals all secrets!"));
    adw_banner_set_revealed (ADW_BANNER (self->plain_warning), TRUE);
    gtk_box_append (GTK_BOX (box), self->plain_warning);

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
    adw_combo_row_set_selected (ADW_COMBO_ROW (self->format_combo), EXPORT_FMT_AEGIS_ENC);

    GtkWidget *scrolled = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scrolled), TRUE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), clamp);
    gtk_widget_set_vexpand (scrolled, TRUE);
    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), scrolled);
    adw_dialog_set_child (ADW_DIALOG (self), toolbar_view);

    g_signal_connect (self->password_row, "changed", G_CALLBACK (on_password_changed), self);
    g_signal_connect (self->password_confirm_row, "changed", G_CALLBACK (on_password_changed), self);
    sensitive_dialog_setup (ADW_DIALOG (self), clear_selected_tokens);
    return self;
}

ExportDialog *
export_dialog_new (DatabaseData *db_data,
                   GtkWidget    *parent)
{
    json_t *empty = json_array ();
    ExportDialog *self = export_dialog_new_selected (db_data, parent, empty);
    json_decref (empty);
    if (self != NULL) {
        g_clear_pointer (&self->tokens_override, json_decref);
        g_object_set (self, "title", _("Export Tokens"), NULL);
    }
    return self;
}
