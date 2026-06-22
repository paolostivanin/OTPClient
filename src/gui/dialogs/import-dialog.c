#define _DEFAULT_SOURCE
#include <string.h>
#include <glib/gi18n.h>
#include "import-dialog.h"
#include "import-export.h"
#include "db-common.h"
#include "common.h"
#include "file-size.h"
#include "gquarks.h"
#include "../qrcode-parser.h"
#include "../google-migration.h"
#include "parse-uri.h"
#include "../otpclient-application.h"

struct _ImportDialog
{
    AdwDialog parent;

    DatabaseData *db_data;
    GtkWidget *parent_widget;
    ImportCallback callback;
    gpointer callback_data;

    GtkWidget *format_combo;
    GtkWidget *password_row;
    GtkWidget *import_button;
    GtkWidget *error_label;

    gchar *selected_file;
    gchar *import_password;
};

G_DEFINE_FINAL_TYPE (ImportDialog, import_dialog, ADW_TYPE_DIALOG)

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

/* Per-format filter spec: NULL pattern lists fall back to "All files".
 * Patterns are case-insensitive thanks to gtk_file_filter_add_suffix. */
static const struct {
    const gchar *label;
    const gchar *action_name;
    gboolean needs_password;
    const gchar *filter_label;          /* shown in the file picker dropdown */
    const gchar *filter_suffixes[3];    /* NULL-terminated; e.g. {"json", NULL} */
} import_formats[] = {
    { "FreeOTP+ (Plain)",       FREEOTPPLUS_PLAIN_ACTION_NAME,  FALSE, "Plain text (*.txt)",   {"txt", NULL, NULL} },
    { "Aegis (Plain JSON)",     AEGIS_PLAIN_ACTION_NAME,        FALSE, "JSON (*.json)",        {"json", NULL, NULL} },
    { "Aegis (Encrypted)",      AEGIS_ENC_ACTION_NAME,          TRUE,  "JSON (*.json)",        {"json", NULL, NULL} },
    { "AuthPro (Plain JSON)",   AUTHPRO_PLAIN_ACTION_NAME,      FALSE, "JSON (*.json)",        {"json", NULL, NULL} },
    { "AuthPro (Encrypted)",    AUTHPRO_ENC_ACTION_NAME,        TRUE,  "Encrypted backup",     {"bin", "dat", NULL} },
    { "2FAS (Plain JSON)",      TWOFAS_PLAIN_ACTION_NAME,       FALSE, "2FAS backup (*.2fas)", {"2fas", "json", NULL} },
    { "2FAS (Encrypted)",       TWOFAS_ENC_ACTION_NAME,         TRUE,  "2FAS backup (*.2fas)", {"2fas", "json", NULL} },
    { "Google QR (File)",       GOOGLE_FILE_ACTION_NAME,        FALSE, "Image (PNG/JPEG)",     {"png", "jpg", "jpeg"} },
};

#define N_IMPORT_FORMATS G_N_ELEMENTS (import_formats)

static void
on_format_changed (AdwComboRow  *combo_row,
                   GParamSpec   *pspec,
                   ImportDialog *self)
{
    (void) pspec;
    guint selected = adw_combo_row_get_selected (combo_row);
    gboolean needs_password = (selected < N_IMPORT_FORMATS &&
                                import_formats[selected].needs_password);
    gtk_widget_set_visible (self->password_row, needs_password);
}

static void
do_import (ImportDialog *self)
{
    guint fmt_idx = adw_combo_row_get_selected (ADW_COMBO_ROW (self->format_combo));
    if (fmt_idx >= N_IMPORT_FORMATS)
        return;

    if (self->import_password != NULL) {
        explicit_bzero (self->import_password, strlen (self->import_password));
        gcry_free (self->import_password);
        self->import_password = NULL;
    }
    if (gtk_widget_get_visible (self->password_row)) {
        const gchar *text = gtk_editable_get_text (GTK_EDITABLE (self->password_row));
        gsize len = strlen (text);
        self->import_password = gcry_calloc_secure (len + 1, 1);
        if (self->import_password == NULL) {
            wipe_password_row (self->password_row);
            gtk_label_set_text (GTK_LABEL (self->error_label),
                                _("Secure memory is exhausted"));
            gtk_widget_set_visible (self->error_label, TRUE);
            return;
        }
        memcpy (self->import_password, text, len);
        wipe_password_row (self->password_row);
    }

    goffset file_size = get_file_size (self->selected_file);
    if (file_size <= 0) {
        gtk_label_set_text (GTK_LABEL (self->error_label),
                            _("Selected file is empty or no longer exists."));
        gtk_widget_set_visible (self->error_label, TRUE);
        return;
    }

    GError *err = NULL;
    GSList *otps = NULL;
    guint qr_invalid = 0;
    guint qr_batch_size = 0;
    guint qr_batch_index = 0;
    if (g_strcmp0 (import_formats[fmt_idx].action_name,
                   GOOGLE_FILE_ACTION_NAME) == 0) {
        gchar *uri = qrcode_parse_image_file (self->selected_file, &err);
        if (uri != NULL) {
            if (g_str_has_prefix (uri, "otpauth-migration://"))
                otps = google_migration_decode (uri, &qr_invalid,
                                                &qr_batch_size, &qr_batch_index,
                                                &err);
            else
                set_otps_from_uris (uri, &otps);
            sensitive_g_free (uri);
            if (otps == NULL && err == NULL)
                g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE,
                             "QR code contains no valid OTP token.");
        }
    } else {
        otps = get_data_from_provider (import_formats[fmt_idx].action_name,
                                       self->selected_file,
                                       self->import_password,
                                       self->db_data->max_file_size_from_memlock,
                                       (gsize) file_size,
                                       &err);
    }

    if (err != NULL)
    {
        gtk_label_set_text (GTK_LABEL (self->error_label), err->message);
        gtk_widget_set_visible (self->error_label, TRUE);
        g_clear_error (&err);
        return;
    }

    ImportSummary summary = {0};
    summary.batch_size = qr_batch_size;
    summary.batch_index = qr_batch_index;
    if (otps != NULL)
    {
        GApplication *default_app = g_application_get_default ();
        OTPClientApplication *app = OTPCLIENT_IS_APPLICATION (default_app)
            ? OTPCLIENT_APPLICATION (default_app)
            : NULL;
        if (app == NULL || otpclient_application_get_app_locked (app) ||
            otpclient_application_get_db_data (app) != self->db_data)
        {
            gtk_label_set_text (GTK_LABEL (self->error_label),
                                _("The active database changed. Reopen this dialog before importing."));
            gtk_widget_set_visible (self->error_label, TRUE);
            free_otps_gslist (otps, g_slist_length (otps));
            return;
        }

        OtpImportReport report = {0, 0, qr_invalid};
        db_import_otps (self->db_data, otps, &report, &err);
        if (err != NULL)
        {
            gtk_label_set_text (GTK_LABEL (self->error_label), err->message);
            gtk_widget_set_visible (self->error_label, TRUE);
            g_clear_error (&err);
            /* update_db has already consumed data_to_add AND rolled
             * in_memory_json_data back to disk state, so no extra cleanup
             * is needed here beyond the otps list (parsed but not committed). */
            free_otps_gslist (otps, g_slist_length (otps));
            return;
        }

        summary.added = report.added;
        summary.skipped_duplicates = report.skipped_duplicates;
        summary.skipped_invalid = report.skipped_invalid;
        summary.skipped = report.skipped_duplicates + report.skipped_invalid;

        guint list_len = g_slist_length (otps);
        free_otps_gslist (otps, list_len);
    }

    adw_dialog_close (ADW_DIALOG (self));

    if (self->callback != NULL)
        self->callback (&summary, self->callback_data);
}

static void
on_file_dialog_open_complete (GObject      *source,
                              GAsyncResult *result,
                              gpointer      user_data)
{
    g_autoptr (ImportDialog) self = IMPORT_DIALOG (user_data);
    GtkFileDialog *dialog = GTK_FILE_DIALOG (source);

    GError *err = NULL;
    GFile *file = gtk_file_dialog_open_finish (dialog, result, &err);
    if (file == NULL)
    {
        g_clear_error (&err);
        return;
    }

    g_free (self->selected_file);
    self->selected_file = g_file_get_path (file);
    g_object_unref (file);

    do_import (self);
}

static void
on_import_clicked (GtkButton    *button,
                   ImportDialog *self)
{
    (void) button;

    GtkFileDialog *dialog = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dialog, _("Select file to import"));

    guint fmt_idx = adw_combo_row_get_selected (ADW_COMBO_ROW (self->format_combo));
    if (fmt_idx < N_IMPORT_FORMATS && import_formats[fmt_idx].filter_label != NULL) {
        g_autoptr (GtkFileFilter) filter = gtk_file_filter_new ();
        gtk_file_filter_set_name (filter, _(import_formats[fmt_idx].filter_label));
        for (guint i = 0; i < G_N_ELEMENTS (import_formats[fmt_idx].filter_suffixes); i++) {
            const gchar *suffix = import_formats[fmt_idx].filter_suffixes[i];
            if (suffix == NULL) break;
            gtk_file_filter_add_suffix (filter, suffix);
        }

        g_autoptr (GtkFileFilter) all_filter = gtk_file_filter_new ();
        gtk_file_filter_set_name (all_filter, _("All files"));
        gtk_file_filter_add_pattern (all_filter, "*");

        g_autoptr (GListStore) filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
        g_list_store_append (filters, filter);
        g_list_store_append (filters, all_filter);
        gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
        gtk_file_dialog_set_default_filter (dialog, filter);
    }

    GtkWindow *win = GTK_WINDOW (gtk_widget_get_root (self->parent_widget));
    gtk_file_dialog_open (dialog, win, NULL,
                          on_file_dialog_open_complete, g_object_ref (self));
    g_object_unref (dialog);
}

static void
import_dialog_finalize (GObject *object)
{
    ImportDialog *self = IMPORT_DIALOG (object);
    wipe_password_row (self->password_row);
    g_free (self->selected_file);
    g_clear_pointer (&self->db_data, database_data_free);
    if (self->import_password != NULL) {
        explicit_bzero (self->import_password, strlen (self->import_password));
        gcry_free (self->import_password);
    }
    G_OBJECT_CLASS (import_dialog_parent_class)->finalize (object);
}

static void
import_dialog_init (ImportDialog *self)
{
    (void) self;
}

static void
import_dialog_class_init (ImportDialogClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->finalize = import_dialog_finalize;
}

ImportDialog *
import_dialog_new (DatabaseData   *db_data,
                   GtkWidget      *parent,
                   ImportCallback  callback,
                   gpointer        user_data)
{
    ImportDialog *self = g_object_new (IMPORT_TYPE_DIALOG,
                                       "title", _("Import Tokens"),
                                       "content-width", 440,
                                       "content-height", 360,
                                       NULL);

    self->db_data = database_data_ref (db_data);
    self->parent_widget = parent;
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

    GtkWidget *group = adw_preferences_group_new ();

    /* Format selector */
    const char *format_names[N_IMPORT_FORMATS + 1];
    for (guint i = 0; i < N_IMPORT_FORMATS; i++)
        format_names[i] = import_formats[i].label;
    format_names[N_IMPORT_FORMATS] = NULL;

    GtkStringList *format_model = gtk_string_list_new (format_names);
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

    gtk_box_append (GTK_BOX (box), group);

    /* Error label */
    self->error_label = gtk_label_new (NULL);
    gtk_widget_add_css_class (self->error_label, "error");
    gtk_widget_set_visible (self->error_label, FALSE);
    gtk_box_append (GTK_BOX (box), self->error_label);

    /* Import button */
    self->import_button = gtk_button_new_with_label (_("Select File & Import"));
    gtk_widget_add_css_class (self->import_button, "suggested-action");
    gtk_widget_add_css_class (self->import_button, "pill");
    gtk_widget_set_halign (self->import_button, GTK_ALIGN_CENTER);
    g_signal_connect (self->import_button, "clicked", G_CALLBACK (on_import_clicked), self);
    gtk_box_append (GTK_BOX (box), self->import_button);

    GtkWidget *scrolled = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scrolled), TRUE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), clamp);
    gtk_widget_set_vexpand (scrolled, TRUE);
    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), scrolled);
    adw_dialog_set_child (ADW_DIALOG (self), toolbar_view);

    return self;
}
