#define _DEFAULT_SOURCE
#include "sensitive-dialog.h"
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
#include "otp-validation.h"
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
    GtkWidget *preview_group;
    GtkWidget *preview_summary;
    GSList *pending_otps;
    GPtrArray *preview_checks;
    gchar *pending_details;
    guint pending_batch_size;
    guint pending_batch_index;
    guint pending_parse_skipped_invalid;

    gchar *selected_file;
    gchar *import_password;
    gboolean import_running;
};

typedef struct {
    gchar *path;
    gchar *action_name;
    gchar *password;
    gint32 max_file_size;
    guint lock_generation;
} ImportParseData;

typedef struct {
    GSList *otps;
    OtpImportDiagnostics *diagnostics;
    GError *error;
    guint qr_invalid;
    guint qr_batch_size;
    guint qr_batch_index;
} ImportParseResult;

static void
import_parse_data_free (gpointer data)
{
    ImportParseData *pd = data;
    g_free (pd->path);
    g_free (pd->action_name);
    if (pd->password != NULL) {
        explicit_bzero (pd->password, strlen (pd->password));
        gcry_free (pd->password);
    }
    g_free (pd);
}

static void
import_parse_result_free (gpointer data)
{
    ImportParseResult *parsed = data;
    free_otps_gslist (parsed->otps);
    otp_import_diagnostics_free (parsed->diagnostics);
    g_clear_error (&parsed->error);
    g_free (parsed);
}

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

static GPtrArray *
import_hash_bucket (GHashTable *index, json_t *obj, gboolean create)
{
    gpointer key = GUINT_TO_POINTER (json_object_get_hash (obj));
    GPtrArray *bucket = g_hash_table_lookup (index, key);
    if (bucket == NULL && create) {
        bucket = g_ptr_array_new ();
        g_hash_table_insert (index, key, bucket);
    }
    return bucket;
}

static gboolean
import_hash_contains (GHashTable *index, json_t *obj)
{
    GPtrArray *bucket = import_hash_bucket (index, obj, FALSE);
    for (guint i = 0; bucket != NULL && i < bucket->len; i++)
        if (json_equal (g_ptr_array_index (bucket, i), obj))
            return TRUE;
    return FALSE;
}

static gchar *
otp_identity_key (const otp_t *otp)
{
    g_autofree gchar *account = g_utf8_casefold (otp->account_name != NULL ? otp->account_name : "", -1);
    g_autofree gchar *issuer = g_utf8_casefold (otp->issuer != NULL ? otp->issuer : "", -1);
    return g_strdup_printf ("%s\037%s", issuer, account);
}

static void
free_pending_preview (ImportDialog *self)
{
    if (self->preview_checks != NULL) {
        g_ptr_array_unref (self->preview_checks);
        self->preview_checks = NULL;
    }
    if (self->pending_otps != NULL) {
        free_otps_gslist (self->pending_otps);
        self->pending_otps = NULL;
    }
    g_clear_pointer (&self->pending_details, g_free);
    self->pending_batch_size = self->pending_batch_index = 0;
    self->pending_parse_skipped_invalid = 0;
}

static void
clear_preview (ImportDialog *self)
{
    free_pending_preview (self);
    if (self->preview_group != NULL) {
        GtkWidget *parent = gtk_widget_get_parent (self->preview_group);
        if (GTK_IS_BOX (parent))
            gtk_box_remove (GTK_BOX (parent), self->preview_group);
        self->preview_group = NULL;
        self->preview_summary = NULL;
    }
    if (GTK_IS_BUTTON (self->import_button))
        gtk_button_set_label (GTK_BUTTON (self->import_button), _("Select File & Preview"));
}

static void
show_import_preview (ImportDialog *self, GSList *otps, const gchar *details,
                     guint batch_size, guint batch_index, guint parse_skipped_invalid)
{
    clear_preview (self);
    self->pending_otps = otps;
    self->pending_details = g_strdup (details);
    self->pending_batch_size = batch_size;
    self->pending_batch_index = batch_index;
    self->pending_parse_skipped_invalid = parse_skipped_invalid;
    self->preview_checks = g_ptr_array_new ();

    self->preview_group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (self->preview_group), _("Review tokens"));
    self->preview_summary = gtk_label_new (NULL);
    gtk_label_set_wrap (GTK_LABEL (self->preview_summary), TRUE);
    gtk_widget_set_halign (self->preview_summary, GTK_ALIGN_START);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->preview_group), self->preview_summary);

    GHashTable *exact = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
                                               (GDestroyNotify) g_ptr_array_unref);
    GHashTable *identities = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                                   (GDestroyNotify) g_ptr_array_unref);
    json_t *tokens = self->db_data->in_memory_json_data;
    if (json_is_array (tokens)) {
        size_t i;
        json_t *obj;
        json_array_foreach (tokens, i, obj) {
            g_ptr_array_add (import_hash_bucket (exact, obj, TRUE), obj);
            const gchar *account = json_string_value (json_object_get (obj, "label"));
            const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
            g_autofree gchar *folded_account = g_utf8_casefold (account != NULL ? account : "", -1);
            g_autofree gchar *folded_issuer = g_utf8_casefold (issuer != NULL ? issuer : "", -1);
            gchar *identity = g_strdup_printf ("%s\037%s", folded_issuer, folded_account);
            GPtrArray *bucket = g_hash_table_lookup (identities, identity);
            if (bucket == NULL) {
                bucket = g_ptr_array_new ();
                g_hash_table_insert (identities, identity, bucket);
            } else {
                g_free (identity);
            }
            g_ptr_array_add (bucket, obj);
        }
    }

    guint exact_count = 0, conflict_count = 0;
    guint index = 0;
    GPtrArray *candidate_objects = g_ptr_array_new_with_free_func ((GDestroyNotify) json_decref);
    for (GSList *l = otps; l != NULL; l = l->next, index++) {
        otp_t *otp = l->data;
        otp_repair_anonymous_import_token (otp, index);
        json_t *candidate = build_json_obj (otp->type, otp->account_name, otp->issuer,
                                            otp->secret, otp->digits, otp->algo,
                                            otp->period, otp->counter, otp->group);
        gboolean duplicate = candidate != NULL && import_hash_contains (exact, candidate);
        gboolean conflict = FALSE;
        if (candidate != NULL && !duplicate) {
            g_autofree gchar *identity = otp_identity_key (otp);
            GPtrArray *bucket = g_hash_table_lookup (identities, identity);
            for (guint n = 0; bucket != NULL && n < bucket->len; n++) {
                json_t *old = g_ptr_array_index (bucket, n);
                const gchar *old_secret = json_string_value (json_object_get (old, "secret"));
                if (g_strcmp0 (old_secret, otp->secret) != 0) {
                    conflict = TRUE;
                    break;
                }
            }
        }
        if (candidate != NULL) {
            g_ptr_array_add (candidate_objects, candidate);
            g_ptr_array_add (import_hash_bucket (exact, candidate, TRUE), candidate);
            if (!duplicate) {
                g_autofree gchar *identity = otp_identity_key (otp);
                GPtrArray *bucket = g_hash_table_lookup (identities, identity);
                if (bucket == NULL) {
                    bucket = g_ptr_array_new ();
                    g_hash_table_insert (identities, g_strdup (identity), bucket);
                }
                g_ptr_array_add (bucket, candidate);
            }
        }
        const gchar *name = (otp->issuer != NULL && otp->issuer[0] != '\0')
            ? otp->issuer : (otp->account_name != NULL ? otp->account_name : _("Unnamed token"));
        g_autofree gchar *label = g_strdup_printf ("%s — %s%s", name,
            otp->account_name != NULL ? otp->account_name : _("No account"),
            duplicate ? _(" (exact duplicate)") : conflict ? _(" (same account, different secret)") : "");
        GtkWidget *check = gtk_check_button_new_with_label (label);
        g_object_set_data (G_OBJECT (check), "exact-duplicate", GUINT_TO_POINTER (duplicate));
        gtk_check_button_set_active (GTK_CHECK_BUTTON (check), !duplicate);
        /* db_import_otps always drops an exact duplicate, so ticking it would
         * promise an import that cannot happen. */
        gtk_widget_set_sensitive (check, !duplicate);
        gtk_widget_set_tooltip_text (check, duplicate
            ? _("This token already exists and will be skipped.")
            : conflict ? _("An existing token has the same issuer and account but a different secret. Review before importing.")
                       : _("This token will be added if selected."));
        adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->preview_group), check);
        g_ptr_array_add (self->preview_checks, check);
        exact_count += duplicate;
        conflict_count += conflict;
    }
    g_hash_table_unref (exact);
    g_hash_table_unref (identities);
    g_ptr_array_unref (candidate_objects);

    g_autofree gchar *summary = g_strdup_printf (
        _("%u tokens found. %u exact duplicates are unchecked; %u possible account conflicts need review.%s"),
        g_slist_length (otps), exact_count, conflict_count,
        details != NULL && details[0] != '\0' ? _(" Some source entries could not be imported; see the details below.") : "");
    gtk_label_set_text (GTK_LABEL (self->preview_summary), summary);
    if (details != NULL && details[0] != '\0')
        gtk_label_set_text (GTK_LABEL (self->error_label), details);
    gtk_widget_set_visible (self->error_label, details != NULL && details[0] != '\0');
    GtkWidget *box = gtk_widget_get_parent (self->import_button);
    gtk_box_insert_child_after (GTK_BOX (box), self->preview_group, self->error_label);
    gtk_button_set_label (GTK_BUTTON (self->import_button), _("Import Selected"));
}

#ifdef OTPCLIENT_TESTING
void
import_dialog_test_preview (ImportDialog *self, GSList *otps)
{
    show_import_preview (self, otps, "", 0, 0, 0);
}
#endif

static void
commit_import_preview (ImportDialog *self)
{
    GApplication *default_app = g_application_get_default ();
    OTPClientApplication *app = OTPCLIENT_IS_APPLICATION (default_app)
        ? OTPCLIENT_APPLICATION (default_app) : NULL;
    if (app == NULL || otpclient_application_get_app_locked (app) ||
        otpclient_application_is_database_operation_in_progress (app) ||
        otpclient_application_get_db_data (app) != self->db_data) {
        gtk_label_set_text (GTK_LABEL (self->error_label),
                            _("The active database changed. Reopen this dialog before importing."));
        gtk_widget_set_visible (self->error_label, TRUE);
        return;
    }
    GSList *selected = NULL;
    guint unchecked = 0, unchecked_duplicates = 0;
    guint i = 0;
    for (GSList *l = self->pending_otps; l != NULL; l = l->next, i++) {
        if (gtk_check_button_get_active (g_ptr_array_index (self->preview_checks, i)))
            selected = g_slist_prepend (selected, l->data);
        else {
            unchecked++;
            if (GPOINTER_TO_UINT (g_object_get_data (
                    G_OBJECT (g_ptr_array_index (self->preview_checks, i)), "exact-duplicate")))
                unchecked_duplicates++;
        }
    }
    selected = g_slist_reverse (selected);
    GError *err = NULL;
    OtpImportReport report = {0};
    if (selected != NULL && !db_import_otps (self->db_data, selected, &report, &err)) {
        gtk_label_set_text (GTK_LABEL (self->error_label), err->message);
        gtk_widget_set_visible (self->error_label, TRUE);
        g_clear_error (&err);
        g_slist_free (selected);
        return;
    }
    ImportSummary summary = {0};
    summary.added = report.added;
    summary.skipped_duplicates = report.skipped_duplicates + unchecked_duplicates;
    summary.skipped_invalid = report.skipped_invalid + self->pending_parse_skipped_invalid;
    summary.skipped_unselected = unchecked - unchecked_duplicates;
    summary.skipped = summary.skipped_duplicates + summary.skipped_invalid + summary.skipped_unselected;
    summary.details = self->pending_details;
    summary.batch_size = self->pending_batch_size;
    summary.batch_index = self->pending_batch_index;
    g_slist_free (selected);
    if (self->callback != NULL)
        self->callback (&summary, self->callback_data);
    /* The closed handler clears the preview; touching self after the close
     * would only be safe if the dialog were disposed synchronously. */
    adw_dialog_close (ADW_DIALOG (self));
}

static void
import_parse_thread (GTask *task, gpointer source, gpointer task_data,
                     GCancellable *cancellable)
{
    (void) source;
    ImportParseData *pd = task_data;
    ImportParseResult *parsed = g_new0 (ImportParseResult, 1);
    parsed->diagnostics = otp_import_diagnostics_new ();
    if (parsed->error == NULL &&
        g_cancellable_set_error_if_cancelled (cancellable, &parsed->error)) {
        g_task_return_pointer (task, parsed, import_parse_result_free);
        return;
    }

    goffset file_size = get_file_size (pd->path);
    if (file_size <= 0) {
        g_set_error_literal (&parsed->error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                             _("Selected file is empty or no longer exists."));
    } else if (g_strcmp0 (pd->action_name, GOOGLE_FILE_ACTION_NAME) == 0) {
        gchar *uri = qrcode_parse_image_file (pd->path, &parsed->error);
        if (uri != NULL) {
            if (g_str_has_prefix (uri, "otpauth-migration://"))
                parsed->otps = google_migration_decode_full (
                    uri, &parsed->qr_invalid, &parsed->qr_batch_size,
                    &parsed->qr_batch_index, parsed->diagnostics, &parsed->error);
            else
                set_otps_from_uris_full (uri, &parsed->otps, parsed->diagnostics);
            sensitive_g_free (uri);
        }
    } else {
        parsed->otps = get_data_from_provider_full (
            pd->action_name, pd->path, pd->password, pd->max_file_size,
            (gsize) file_size, parsed->diagnostics, &parsed->error);
    }
    if (parsed->error == NULL &&
        g_cancellable_set_error_if_cancelled (cancellable, &parsed->error)) {
        g_task_return_pointer (task, parsed, import_parse_result_free);
        return;
    }
    if (parsed->otps == NULL && parsed->error == NULL)
        g_set_error_literal (&parsed->error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                             _("No valid tokens found; nothing was imported."));
    g_task_return_pointer (task, parsed, import_parse_result_free);
}

static void
import_parse_done (GObject *source, GAsyncResult *result, gpointer user_data)
{
    (void) source;
    /* The task holds no reference to the dialog, so its last unref on the
     * worker cannot dispose GTK widgets there. This callback runs on the main
     * thread and drops the reference it was given. */
    g_autoptr (ImportDialog) self = user_data;
    self->import_running = FALSE;
    gtk_widget_set_sensitive (self->format_combo, TRUE);
    gtk_widget_set_sensitive (self->password_row, TRUE);
    gtk_button_set_label (GTK_BUTTON (self->import_button), _("Import"));
    gtk_widget_set_sensitive (self->import_button, TRUE);
    if (sensitive_dialog_is_closed (ADW_DIALOG (self)))
        return;

    GError *task_error = NULL;
    ImportParseResult *parsed = g_task_propagate_pointer (G_TASK (result), &task_error);
    if (task_error != NULL) {
        if (!g_error_matches (task_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            gtk_label_set_text (GTK_LABEL (self->error_label), task_error->message);
            gtk_widget_set_visible (self->error_label, TRUE);
        }
        g_clear_error (&task_error);
        return;
    }

    GApplication *default_app = g_application_get_default ();
    OTPClientApplication *app = OTPCLIENT_IS_APPLICATION (default_app)
        ? OTPCLIENT_APPLICATION (default_app) : NULL;
    ImportParseData *pd = g_task_get_task_data (G_TASK (result));
    if (app == NULL || otpclient_application_get_app_locked (app) ||
        otpclient_application_is_database_operation_in_progress (app) ||
        otpclient_application_get_db_data (app) != self->db_data ||
        otpclient_application_get_lock_generation (app) != pd->lock_generation) {
        gtk_label_set_text (GTK_LABEL (self->error_label),
                            _("The active database changed. Reopen this dialog before importing."));
        gtk_widget_set_visible (self->error_label, TRUE);
        import_parse_result_free (parsed);
        return;
    }

    g_autofree gchar *details = otp_import_diagnostics_format (parsed->diagnostics);
    if (parsed->error != NULL) {
        g_autofree gchar *message = g_strconcat (
            parsed->error->message, details[0] != '\0' ? "\n" : "", details, NULL);
        gtk_label_set_text (GTK_LABEL (self->error_label), message);
        gtk_widget_set_visible (self->error_label, TRUE);
        import_parse_result_free (parsed);
        return;
    }
    show_import_preview (self, parsed->otps, details, parsed->qr_batch_size,
                         parsed->qr_batch_index,
                         parsed->diagnostics->skipped_invalid + parsed->qr_invalid);
    parsed->otps = NULL;
    import_parse_result_free (parsed);
}

static void
do_import (ImportDialog *self)
{
    if (self->pending_otps != NULL) {
        commit_import_preview (self);
        return;
    }
    if (self->import_running)
        return;
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

    ImportParseData *pd = g_new0 (ImportParseData, 1);
    pd->path = g_strdup (self->selected_file);
    pd->action_name = g_strdup (import_formats[fmt_idx].action_name);
    pd->max_file_size = self->db_data->max_file_size_from_memlock;
    GApplication *default_app = g_application_get_default ();
    OTPClientApplication *app = OTPCLIENT_IS_APPLICATION (default_app)
        ? OTPCLIENT_APPLICATION (default_app) : NULL;
    pd->lock_generation = app != NULL
        ? otpclient_application_get_lock_generation (app) : 0;
    if (self->import_password != NULL) {
        gsize len = strlen (self->import_password);
        pd->password = gcry_calloc_secure (len + 1, 1);
        if (pd->password == NULL) {
            import_parse_data_free (pd);
            explicit_bzero (self->import_password, strlen (self->import_password));
            gcry_free (self->import_password);
            self->import_password = NULL;
            gtk_label_set_text (GTK_LABEL (self->error_label),
                                _("Secure memory is exhausted"));
            gtk_widget_set_visible (self->error_label, TRUE);
            return;
        }
        memcpy (pd->password, self->import_password, len);
        explicit_bzero (self->import_password, strlen (self->import_password));
        gcry_free (self->import_password);
        self->import_password = NULL;
    }

    self->import_running = TRUE;
    gtk_widget_set_sensitive (self->format_combo, FALSE);
    gtk_widget_set_sensitive (self->password_row, FALSE);
    gtk_button_set_label (GTK_BUTTON (self->import_button), _("Importing…"));
    gtk_widget_set_sensitive (self->import_button, FALSE);
    gtk_widget_set_visible (self->error_label, FALSE);
    GTask *task = g_task_new (NULL, sensitive_dialog_get_cancellable (ADW_DIALOG (self)),
                              import_parse_done, g_object_ref (self));
    g_task_set_task_data (task, pd, import_parse_data_free);
    g_task_run_in_thread (task, import_parse_thread);
    g_object_unref (task);
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
    if (sensitive_dialog_is_closed (ADW_DIALOG (self))) {
        g_clear_object (&file);
        g_clear_error (&err);
        return;
    }
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

    if (self->pending_otps != NULL) {
        commit_import_preview (self);
        return;
    }

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
    gtk_file_dialog_open (dialog, win, sensitive_dialog_get_cancellable (ADW_DIALOG (self)),
                          on_file_dialog_open_complete, g_object_ref (self));
    g_object_unref (dialog);
}

static void
clear_import_password (AdwDialog *dialog)
{
    ImportDialog *self = IMPORT_DIALOG (dialog);
    if (self->import_password != NULL) {
        explicit_bzero (self->import_password, strlen (self->import_password));
        g_clear_pointer (&self->import_password, gcry_free);
    }
    clear_preview (self);
}

static void
import_dialog_finalize (GObject *object)
{
    ImportDialog *self = IMPORT_DIALOG (object);
    free_pending_preview (self);
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
        format_names[i] = _(import_formats[i].label);
    format_names[N_IMPORT_FORMATS] = NULL;

    GtkStringList *format_model = gtk_string_list_new (format_names);
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

    gtk_box_append (GTK_BOX (box), group);

    /* Error label */
    self->error_label = gtk_label_new (NULL);
    gtk_label_set_wrap (GTK_LABEL (self->error_label), TRUE);
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

    sensitive_dialog_setup (ADW_DIALOG (self), clear_import_password);
    return self;
}
