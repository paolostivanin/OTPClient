#include <string.h>
#include <stdarg.h>
#include <libsecret/secret.h>
#include "review-fixture.h"
#include "otpclient-application.h"
#include "otpclient-window-private.h"
#include "otp-entry.h"
#include "otp-validation.h"
#include "gsettings-common.h"
#include "parse-uri.h"
#include "lock-app.h"
#include "dialogs/qr-display-dialog.h"
#include "dialogs/password-dialog.h"
#include "dialogs/manual-add-dialog.h"
#include "dialogs/edit-token-dialog.h"
#include "dialogs/import-dialog.h"
#include "dialogs/export-dialog.h"
#include "dialogs/sensitive-dialog.h"
#include "autostart.h"
#include "version.h"

static OTPClientApplication *app;
static OTPClientWindow *win;

static guint password_store_calls;
static gchar *stored_password;
static gchar *stored_password_path;
static gboolean delay_password_store;
static gboolean fail_password_store;
static GTask *pending_password_store;
static guint password_store_completions;
static gint password_lookup_calls;
static gboolean provide_lookup_password;

gchar *
__wrap_secret_password_lookup_sync (const SecretSchema *schema,
                                    GCancellable *cancellable,
                                    GError **error, ...)
{
    (void) schema;
    (void) cancellable;
    (void) error;
    g_atomic_int_inc (&password_lookup_calls);
    return provide_lookup_password ? g_strdup ("test-password") : NULL;
}

/* Keep the worker, completion callback, and UI real; replace only the external
 * keyring boundary. The isolated session deliberately has no Secret Service. */
void
__wrap_secret_password_store (const SecretSchema *schema,
                              const gchar *collection,
                              const gchar *label,
                              const gchar *password,
                              GCancellable *cancellable,
                              GAsyncReadyCallback callback,
                              gpointer user_data, ...)
{
    (void) schema;
    (void) collection;
    (void) label;
    va_list args;
    va_start (args, user_data);
    g_assert_cmpstr (va_arg (args, const gchar *), ==, "string");
    const gchar *path = va_arg (args, const gchar *);
    g_assert_null (va_arg (args, const gchar *));
    g_clear_pointer (&stored_password, sensitive_secure_free);
    g_clear_pointer (&stored_password_path, g_free);
    stored_password = secure_strdup (password);
    stored_password_path = g_strdup (path);
    va_end (args);
    password_store_calls++;
    GTask *task = g_task_new (NULL, cancellable, callback, user_data);
    if (delay_password_store) {
        g_assert_null (pending_password_store);
        pending_password_store = task;
        return;
    }
    g_task_return_boolean (task, TRUE);
    g_object_unref (task);
}

gboolean
__wrap_secret_password_store_finish (GAsyncResult *result, GError **error)
{
    password_store_completions++;
    return g_task_propagate_boolean (G_TASK (result), error);
}

static void
settle (void)
{
    gint64 end = g_get_monotonic_time () + 100000;
    do {
        for (guint i = 0; i < 100 && g_main_context_iteration (NULL, FALSE); i++);
        g_usleep (1000);
    } while (g_get_monotonic_time () < end);
}

static GtkWidget *
find_widget (GtkWidget *parent, GType type, const gchar *title)
{
    if (g_type_is_a (G_OBJECT_TYPE (parent), type)) {
        /* Icon-only buttons, such as the token list's action button, have no
         * label, so fall back to the icon name for those. */
        const gchar *label = GTK_IS_BUTTON (parent)
            ? (gtk_button_get_label (GTK_BUTTON (parent)) ?: gtk_button_get_icon_name (GTK_BUTTON (parent)))
            : ADW_IS_PREFERENCES_ROW (parent) ? adw_preferences_row_get_title (ADW_PREFERENCES_ROW (parent)) : NULL;
        if (title == NULL || g_strcmp0 (title, label) == 0)
            return parent;
    }
    for (GtkWidget *child = gtk_widget_get_first_child (parent); child != NULL;
         child = gtk_widget_get_next_sibling (child)) {
        GtkWidget *found = find_widget (child, type, title);
        if (found != NULL) return found;
    }
    return NULL;
}

static void
collect_entries (GtkWidget *parent, GPtrArray *entries)
{
    if (GTK_IS_ENTRY (parent))
        g_ptr_array_add (entries, parent);
    for (GtkWidget *child = gtk_widget_get_first_child (parent); child != NULL;
         child = gtk_widget_get_next_sibling (child))
        collect_entries (child, entries);
}

static void
collect_check_buttons (GtkWidget *parent, GPtrArray *buttons)
{
    if (GTK_IS_CHECK_BUTTON (parent))
        g_ptr_array_add (buttons, parent);
    for (GtkWidget *child = gtk_widget_get_first_child (parent); child != NULL;
         child = gtk_widget_get_next_sibling (child))
        collect_check_buttons (child, buttons);
}

static void
assert_cleared (GtkWidget *parent)
{
    if (GTK_IS_EDITABLE (parent) && !GTK_IS_SPIN_BUTTON (parent))
        g_assert_cmpstr (gtk_editable_get_text (GTK_EDITABLE (parent)), ==, "");
    if (GTK_IS_PICTURE (parent))
        g_assert_null (gtk_picture_get_paintable (GTK_PICTURE (parent)));
    for (GtkWidget *child = gtk_widget_get_first_child (parent); child != NULL;
         child = gtk_widget_get_next_sibling (child))
        assert_cleared (child);
}

static OTPEntry *
attach_fixture (ReviewFixture *fixture)
{
    review_fixture_init (fixture);
    otpclient_application_set_db_data (app, database_data_ref (fixture->db));
    otpclient_application_set_app_locked (app, FALSE);
    otpclient_window_set_db_actions_enabled (win, TRUE);
    g_list_store_remove_all (win->otp_store);
    OTPEntry *entry = otp_entry_new ("alice", "Example", NULL, "HOTP", 30, 0,
                                    "SHA1", 6, REVIEW_SECRET);
    otp_entry_update_otp (entry);
    g_list_store_append (win->otp_store, entry);
    gtk_single_selection_set_selected (win->otp_selection, 0);
    gtk_window_present (GTK_WINDOW (win));
    settle ();
    return entry;
}

static void
test_hotp_and_clipboard (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    g_assert_cmpuint (otp_entry_get_counter (entry), ==, 0);
    g_assert_null (otp_entry_get_otp_value (entry));
    /* The Action column's button for an HOTP row: icon-only, so it is found by
     * the icon that stands in for "Generate". */
    GtkWidget *button = find_widget (GTK_WIDGET (win), GTK_TYPE_BUTTON, "view-refresh-symbolic");
    g_assert_nonnull (button);
    g_signal_emit_by_name (button, "clicked");
    g_assert_cmpstr (otp_entry_get_otp_value (entry), ==, "755224");
    g_assert_cmpuint (otp_entry_get_counter (entry), ==, 1);
    gtk_single_selection_set_selected (win->otp_selection, GTK_INVALID_LIST_POSITION);
    gtk_single_selection_set_selected (win->otp_selection, 0);
    g_assert_cmpuint (otp_entry_get_counter (entry), ==, 1);

#ifdef REVIEW_CLI_PATH
    const gchar *args[] = {"--show", "--account", "alice", "--output=json", NULL};
    gchar *err = NULL;
    g_autofree gchar *out = review_cli (&fixture, args, TRUE, &err);
    g_assert_cmpstr (err, ==, "");
    g_free (err);
    json_t *rows = json_loads (out, 0, NULL);
    g_assert_nonnull (rows);
    g_assert_cmpstr (json_string_value (json_object_get (json_array_get (rows, 0), "current")), ==, "287082");
    json_decref (rows);
    /* The GUI's stale snapshot must fail without delivering a new code. */
    g_signal_emit_by_name (button, "clicked");
    g_assert_cmpstr (otp_entry_get_otp_value (entry), ==, "755224");
    g_assert_cmpuint (otp_entry_get_counter (entry), ==, 1);
#endif
    GdkClipboard *clipboard = gdk_display_get_clipboard (gdk_display_get_default ());
    g_autoptr (GdkContentProvider) external = gdk_content_provider_new_typed (G_TYPE_STRING, "copied elsewhere");
    gdk_clipboard_set_content (clipboard, external);
    settle ();
    g_assert_null (win->clipboard_content);
    g_assert_cmpuint (win->clipboard_clear_timer_id, ==, 0);
    otpclient_window_clear_clipboard_now (win);
    g_assert_true (gdk_clipboard_get_content (clipboard) == external);
    lock_app_lock (app);
    g_assert_true (gdk_clipboard_get_content (clipboard) == external);
    AdwDialog *unlock = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win));
    if (unlock != NULL) adw_dialog_force_close (unlock);
    review_fixture_clear (&fixture);
}

static void
test_quarantine_review_can_delete_preserved_token (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    (void) entry;
    fixture.db->quarantined_tokens = json_array ();
    json_t *invalid = build_json_obj ("TOTP", "broken", "Example", REVIEW_SECRET,
                                      6, "SHA1", 30, 0, NULL);
    json_object_set_new (invalid, "digits", json_integer (99));
    json_array_append_new (fixture.db->quarantined_tokens, invalid);
    GError *err = NULL;
    update_db (fixture.db, &err);
    g_assert_no_error (err);

    otpclient_window_database_modified (win);
    g_assert_nonnull (win->quarantine_banner);
    g_assert_true (adw_banner_get_revealed (ADW_BANNER (win->quarantine_banner)));
    GtkWidget *review_button = find_widget (win->quarantine_banner, GTK_TYPE_BUTTON, "Review");
    g_assert_nonnull (review_button);
    g_signal_emit_by_name (review_button, "clicked");
    settle ();
    AdwDialog *review = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win));
    g_assert_nonnull (review);
    GtkWidget *delete_button = find_widget (GTK_WIDGET (review), GTK_TYPE_BUTTON, "Delete");
    g_assert_nonnull (delete_button);

    /* Deleting asks first; cancelling keeps the only copy of the secret. */
    g_signal_emit_by_name (delete_button, "clicked");
    settle ();
    AdwDialog *confirm = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win));
    g_assert_true (confirm != review);
    GtkWidget *cancel = find_widget (GTK_WIDGET (confirm), GTK_TYPE_BUTTON, "Cancel");
    g_assert_nonnull (cancel);
    g_signal_emit_by_name (cancel, "clicked");
    settle ();
    g_assert_cmpuint (db_get_quarantined_count (fixture.db), ==, 1);
    g_assert_true (adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win)) == review);

    g_signal_emit_by_name (delete_button, "clicked");
    settle ();
    confirm = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win));
    g_assert_true (confirm != review);
    GtkWidget *confirm_delete = find_widget (GTK_WIDGET (confirm), GTK_TYPE_BUTTON, "Delete");
    g_assert_nonnull (confirm_delete);
    g_signal_emit_by_name (confirm_delete, "clicked");
    settle ();
    g_assert_cmpuint (db_get_quarantined_count (fixture.db), ==, 0);
    g_assert_false (adw_banner_get_revealed (ADW_BANNER (win->quarantine_banner)));
    g_assert_cmpuint (json_array_size (fixture.db->in_memory_json_data), ==, 1);
    g_assert_null (adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win)));
    review_fixture_clear (&fixture);
}

static void
test_quarantine_review_repairs_and_validates_token (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    (void) entry;
    fixture.db->quarantined_tokens = json_array ();
    json_t *invalid = build_json_obj ("TOTP", "broken", "Example", REVIEW_SECRET,
                                      6, "SHA1", 30, 0, NULL);
    json_object_set_new (invalid, "digits", json_integer (99));
    json_array_append_new (fixture.db->quarantined_tokens, invalid);
    GError *err = NULL;
    update_db (fixture.db, &err);
    g_assert_no_error (err);

    otpclient_window_database_modified (win);
    gtk_widget_activate_action (GTK_WIDGET (win), "win.review-quarantine", NULL);
    settle ();
    AdwDialog *review = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win));
    g_assert_nonnull (review);
    GtkWidget *repair_button = find_widget (GTK_WIDGET (review), GTK_TYPE_BUTTON, "Repair…");
    g_assert_nonnull (repair_button);
    g_signal_emit_by_name (repair_button, "clicked");
    settle ();

    AdwDialog *repair = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win));
    g_assert_nonnull (repair);
    g_autoptr (GPtrArray) entries = g_ptr_array_new ();
    collect_entries (GTK_WIDGET (repair), entries);
    g_assert_cmpuint (entries->len, >=, 6);
    gtk_editable_set_text (GTK_EDITABLE (g_ptr_array_index (entries, 5)), "6");
    GtkWidget *save = find_widget (GTK_WIDGET (repair), GTK_TYPE_BUTTON, "Validate and Save");
    g_assert_nonnull (save);
    g_signal_emit_by_name (save, "clicked");
    settle ();


    g_assert_cmpuint (db_get_quarantined_count (fixture.db), ==, 0);
    g_assert_cmpuint (json_array_size (fixture.db->in_memory_json_data), ==, 2);
    g_assert_true (otp_validate_database_root (fixture.db->in_memory_json_data, &err));
    g_assert_no_error (err);
    review_fixture_clear (&fixture);
}

/* Opens the repair form for a quarantined TOTP whose digits are out of range and
 * returns its text entries: label, issuer, group, type, algorithm, digits, period,
 * counter (the secret is a password entry and is not collected). */
static AdwDialog *
open_quarantine_repair (ReviewFixture *fixture, GPtrArray *entries)
{
    fixture->db->quarantined_tokens = json_array ();
    json_t *invalid = build_json_obj ("TOTP", "broken", "Example", REVIEW_SECRET,
                                      6, "SHA1", 30, 0, NULL);
    json_object_set_new (invalid, "digits", json_integer (99));
    json_array_append_new (fixture->db->quarantined_tokens, invalid);
    GError *err = NULL;
    update_db (fixture->db, &err);
    g_assert_no_error (err);

    otpclient_window_database_modified (win);
    gtk_widget_activate_action (GTK_WIDGET (win), "win.review-quarantine", NULL);
    settle ();
    AdwDialog *review = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win));
    g_assert_nonnull (review);
    GtkWidget *repair_button = find_widget (GTK_WIDGET (review), GTK_TYPE_BUTTON, "Repair…");
    g_assert_nonnull (repair_button);
    g_signal_emit_by_name (repair_button, "clicked");
    settle ();

    AdwDialog *repair = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win));
    g_assert_nonnull (repair);
    g_assert_true (repair != review);
    collect_entries (GTK_WIDGET (repair), entries);
    g_assert_cmpuint (entries->len, ==, 8);
    return repair;
}

static void
test_quarantine_repair_reports_invalid_fields (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    (void) entry;
    g_autoptr (GPtrArray) entries = g_ptr_array_new ();
    AdwDialog *repair = open_quarantine_repair (&fixture, entries);
    gtk_editable_set_text (GTK_EDITABLE (g_ptr_array_index (entries, 5)), "3");
    GtkWidget *save = find_widget (GTK_WIDGET (repair), GTK_TYPE_BUTTON, "Validate and Save");
    g_assert_nonnull (save);
    g_signal_emit_by_name (save, "clicked");
    settle ();

    AdwDialog *error = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win));
    g_assert_true (ADW_IS_ALERT_DIALOG (error));
    g_assert_cmpstr (adw_alert_dialog_get_heading (ADW_ALERT_DIALOG (error)), ==, "Repair failed");
    g_assert_cmpuint (db_get_quarantined_count (fixture.db), ==, 1);
    g_assert_cmpuint (json_array_size (fixture.db->in_memory_json_data), ==, 1);
    GtkWidget *close = find_widget (GTK_WIDGET (error), GTK_TYPE_BUTTON, "Close");
    g_assert_nonnull (close);
    g_signal_emit_by_name (close, "clicked");
    settle ();

    AdwDialog *review = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win));
    g_assert_true (ADW_IS_ALERT_DIALOG (review));
    g_assert_cmpstr (adw_alert_dialog_get_heading (ADW_ALERT_DIALOG (review)), ==, "Tokens requiring repair");
    adw_dialog_force_close (review);
    settle ();
    review_fixture_clear (&fixture);
}

static void
test_quarantine_repair_normalizes_case (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    (void) entry;
    g_autoptr (GPtrArray) entries = g_ptr_array_new ();
    AdwDialog *repair = open_quarantine_repair (&fixture, entries);
    gtk_editable_set_text (GTK_EDITABLE (g_ptr_array_index (entries, 3)), "totp");
    gtk_editable_set_text (GTK_EDITABLE (g_ptr_array_index (entries, 4)), "sha256");
    gtk_editable_set_text (GTK_EDITABLE (g_ptr_array_index (entries, 5)), "6");
    GtkWidget *save = find_widget (GTK_WIDGET (repair), GTK_TYPE_BUTTON, "Validate and Save");
    g_assert_nonnull (save);
    g_signal_emit_by_name (save, "clicked");
    settle ();

    g_assert_cmpuint (db_get_quarantined_count (fixture.db), ==, 0);
    g_assert_cmpuint (json_array_size (fixture.db->in_memory_json_data), ==, 2);
    json_t *repaired = json_array_get (fixture.db->in_memory_json_data, 1);
    g_assert_cmpstr (json_string_value (json_object_get (repaired, "type")), ==, "TOTP");
    g_assert_cmpstr (json_string_value (json_object_get (repaired, "algo")), ==, "SHA256");
    review_fixture_clear (&fixture);
}

static void
test_manage_groups_renames_all_matching_tokens (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    (void) entry;
    json_object_set_new (json_array_get (fixture.db->in_memory_json_data, 0),
                         "group", json_string ("Work"));
    GError *err = NULL;
    update_db (fixture.db, &err);
    g_assert_no_error (err);

    g_assert_true (gtk_widget_activate_action (GTK_WIDGET (win), "win.manage-groups", NULL));
    settle ();
    AdwDialog *dialog = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win));
    g_assert_nonnull (dialog);
    GtkWidget *dropdown = find_widget (GTK_WIDGET (dialog), GTK_TYPE_DROP_DOWN, NULL);
    GtkWidget *entry_widget = find_widget (GTK_WIDGET (dialog), GTK_TYPE_ENTRY, NULL);
    GtkWidget *rename = find_widget (GTK_WIDGET (dialog), GTK_TYPE_BUTTON, "Rename Group");
    g_assert_nonnull (dropdown);
    g_assert_nonnull (entry_widget);
    g_assert_nonnull (rename);
    gtk_editable_set_text (GTK_EDITABLE (entry_widget), "Projects");
    g_signal_emit_by_name (rename, "clicked");
    settle ();
    g_assert_cmpstr (json_string_value (json_object_get (
        json_array_get (fixture.db->in_memory_json_data, 0), "group")), ==, "Projects");
    review_fixture_clear (&fixture);
}

static void
test_bulk_delete_restores_selected_tokens (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    (void) entry;
    GtkWidget *mode = win->bulk_mode_button;
    g_assert_true (GTK_IS_TOGGLE_BUTTON (mode));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (mode), TRUE);
    settle ();
    g_autoptr (GPtrArray) checks = g_ptr_array_new ();
    collect_check_buttons (GTK_WIDGET (win), checks);
    g_assert_cmpuint (checks->len, >, 0);
    gtk_check_button_set_active (g_ptr_array_index (checks, 0), TRUE);
    g_assert_cmpuint (g_hash_table_size (win->bulk_selected_entries), ==, 1);

    g_assert_true (gtk_widget_activate_action (GTK_WIDGET (win), "win.bulk-delete", NULL));
    settle ();
    AdwDialog *confirm = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win));
    g_assert_nonnull (confirm);
    GtkWidget *delete_button = find_widget (GTK_WIDGET (confirm), GTK_TYPE_BUTTON, "Delete");
    g_assert_nonnull (delete_button);
    g_signal_emit_by_name (delete_button, "clicked");
    settle ();
    g_assert_cmpuint (json_array_size (fixture.db->in_memory_json_data), ==, 0);

    g_assert_nonnull (win->undo_actions);
    otpclient_window_undo_latest_delete (win);
    settle ();
    g_assert_cmpuint (json_array_size (fixture.db->in_memory_json_data), ==, 1);
    g_assert_cmpstr (json_string_value (json_object_get (
        json_array_get (fixture.db->in_memory_json_data, 0), "label")), ==, "alice");
    g_assert_cmpstr (json_string_value (json_object_get (
        json_array_get (fixture.db->in_memory_json_data, 0), "secret")), ==, REVIEW_SECRET);
    GError *err = NULL;
    gsize index = 0;
    g_autoptr (GPtrArray) codes = db_generate_hotp (fixture.db, &index, 1, &err);
    g_assert_no_error (err);
    g_assert_nonnull (codes);
    g_assert_cmpstr (((DbHotpResult *) g_ptr_array_index (codes, 0))->code, ==, "755224");
    review_fixture_clear (&fixture);
}

static void
test_single_delete_and_undo_write_failures (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    json_array_append_new (fixture.db->in_memory_json_data,
        build_json_obj ("TOTP", "bob", "Example", REVIEW_SECRET, 6, "SHA1", 30, 0, NULL));
    GError *err = NULL;
    update_db (fixture.db, &err);
    g_assert_no_error (err);

    gtk_widget_activate_action (GTK_WIDGET (win), "win.delete-token", NULL);
    settle ();
    AdwDialog *dialog = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win));
    g_assert_nonnull (dialog);
    GtkWidget *button = find_widget (GTK_WIDGET (dialog), GTK_TYPE_BUTTON, "Delete");
    g_assert_nonnull (button);
    db_test_set_fail_atomic_write (TRUE);
    g_signal_emit_by_name (button, "clicked");
    settle ();
    db_test_set_fail_atomic_write (FALSE);
    g_assert_true (json_equal (fixture.db->in_memory_json_data, fixture.db->committed_json_data));
    g_assert_cmpuint (json_array_size (fixture.db->in_memory_json_data), ==, 2);

    gtk_widget_activate_action (GTK_WIDGET (win), "win.delete-token", NULL);
    settle ();
    dialog = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win));
    button = find_widget (GTK_WIDGET (dialog), GTK_TYPE_BUTTON, "Delete");
    g_assert_nonnull (button);
    g_signal_emit_by_name (button, "clicked");
    settle ();
    g_assert_cmpuint (json_array_size (fixture.db->in_memory_json_data), ==, 1);

    db_test_set_fail_atomic_write (TRUE);
    otpclient_window_undo_latest_delete (win);
    db_test_set_fail_atomic_write (FALSE);
    g_assert_true (json_equal (fixture.db->in_memory_json_data, fixture.db->committed_json_data));
    g_assert_cmpstr (json_string_value (json_object_get (
        json_array_get (fixture.db->in_memory_json_data, 0), "label")), ==, "bob");

    otpclient_window_undo_latest_delete (win);
    g_assert_cmpuint (json_array_size (fixture.db->in_memory_json_data), ==, 2);
    g_assert_cmpstr (json_string_value (json_object_get (
        json_array_get (fixture.db->in_memory_json_data, 0), "label")), ==, "alice");
    g_assert_cmpstr (json_string_value (json_object_get (
        json_array_get (fixture.db->in_memory_json_data, 1), "label")), ==, "bob");
    DatabaseData *reader = database_data_new (fixture.path, DEFAULT_MEMLOCK_VALUE);
    reader->key = secure_strdup ("test-password");
    load_db (reader, &err);
    g_assert_no_error (err);
    g_assert_true (json_equal (reader->in_memory_json_data, fixture.db->in_memory_json_data));
    database_data_free (reader);
    review_fixture_clear (&fixture);
}

static void
test_bulk_selection_mode_refreshes_rows (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    g_assert_false (gtk_column_view_column_get_visible (win->bulk_selection_column));
    for (guint cycle = 0; cycle < 2; cycle++) {
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (win->bulk_mode_button), TRUE);
        settle ();
        g_assert_true (gtk_column_view_column_get_visible (win->bulk_selection_column));
        g_autoptr (GPtrArray) checks = g_ptr_array_new ();
        collect_check_buttons (win->otp_list, checks);
        g_assert_cmpuint (checks->len, >, 0);
        for (guint i = 0; i < checks->len; i++) {
            GtkWidget *check = g_ptr_array_index (checks, i);
            g_assert_true (gtk_widget_get_visible (check));
            g_assert_false (gtk_check_button_get_active (GTK_CHECK_BUTTON (check)));
        }
        gtk_check_button_set_active (g_ptr_array_index (checks, 0), TRUE);
        g_assert_cmpuint (g_hash_table_size (win->bulk_selected_entries), ==, 1);
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (win->bulk_mode_button), FALSE);
        settle ();
        g_assert_false (gtk_column_view_column_get_visible (win->bulk_selection_column));
        g_assert_cmpuint (g_hash_table_size (win->bulk_selected_entries), ==, 0);
    }
    review_fixture_clear (&fixture);
}

static void
test_bulk_selection_follows_filter (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    (void) entry;
    json_array_append_new (fixture.db->in_memory_json_data,
                           build_json_obj ("TOTP", "bob", "Other", REVIEW_SECRET,
                                           6, "SHA1", 30, 0, NULL));
    GError *err = NULL;
    update_db (fixture.db, &err);
    g_assert_no_error (err);
    g_autoptr (OTPEntry) bob = otp_entry_new ("bob", "Other", NULL, "TOTP", 30, 0,
                                              "SHA1", 6, REVIEW_SECRET);
    g_list_store_append (win->otp_store, bob);

    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (win->bulk_mode_button), TRUE);
    settle ();
    g_autoptr (GPtrArray) checks = g_ptr_array_new ();
    collect_check_buttons (win->otp_list, checks);
    g_assert_cmpuint (checks->len, ==, 2);
    for (guint i = 0; i < checks->len; i++)
        gtk_check_button_set_active (g_ptr_array_index (checks, i), TRUE);
    g_assert_cmpuint (g_hash_table_size (win->bulk_selected_entries), ==, 2);

    /* Hiding bob drops him from the selection, so an action cannot touch a
     * token the user no longer sees checked. */
    gtk_editable_set_text (GTK_EDITABLE (win->search_entry), "alice");
    g_signal_emit_by_name (win->search_entry, "search-changed");
    settle ();
    g_assert_cmpuint (g_hash_table_size (win->bulk_selected_entries), ==, 1);
    g_autoptr (GArray) indices = otpclient_window_get_bulk_selected_indices (win, NULL);
    g_assert_cmpuint (indices->len, ==, 1);
    g_assert_cmpuint (g_array_index (indices, guint, 0), ==, 0);

    gtk_editable_set_text (GTK_EDITABLE (win->search_entry), "");
    g_signal_emit_by_name (win->search_entry, "search-changed");
    settle ();
    g_assert_cmpuint (g_hash_table_size (win->bulk_selected_entries), ==, 1);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (win->bulk_mode_button), FALSE);
    settle ();
    review_fixture_clear (&fixture);
}

static void
capture_import_summary (const ImportSummary *summary, gpointer user_data)
{
    ImportSummary *captured = user_data;
    *captured = *summary;
    captured->details = NULL;
}

static void
test_import_preview_duplicates_and_conflicts (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    json_t *existing = json_array_get (fixture.db->in_memory_json_data, 0);
    json_t *type = json_incref (json_object_get (existing, "type"));
    json_object_del (existing, "type");
    json_object_set_new (existing, "type", type);
    GError *err = NULL;
    update_db (fixture.db, &err);
    g_assert_no_error (err);

    ImportSummary summary = {0};
    ImportDialog *dialog = import_dialog_new (fixture.db, GTK_WIDGET (win),
                                               capture_import_summary, &summary);
    g_object_ref_sink (dialog);
    adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (win));
    GSList *otps = NULL;
    set_otps_from_uris (
        "otpauth://hotp/Example:alice?secret=" REVIEW_SECRET "&issuer=Example&counter=0\n"
        "otpauth://hotp/Example:alice?secret=JBSWY3DPEHPK3PXP&issuer=Example&counter=0\n"
        "otpauth://totp/Example:bob?secret=" REVIEW_SECRET "&issuer=Example\n"
        "otpauth://totp/Example:bob?secret=" REVIEW_SECRET "&issuer=Example",
        &otps);
    g_assert_cmpuint (g_slist_length (otps), ==, 4);
    import_dialog_test_preview (dialog, otps);
    g_autoptr (GPtrArray) checks = g_ptr_array_new ();
    collect_check_buttons (GTK_WIDGET (dialog), checks);
    g_assert_cmpuint (checks->len, ==, 4);
    g_assert_false (gtk_check_button_get_active (g_ptr_array_index (checks, 0)));
    g_assert_true (gtk_check_button_get_active (g_ptr_array_index (checks, 1)));
    g_assert_true (gtk_check_button_get_active (g_ptr_array_index (checks, 2)));
    g_assert_false (gtk_check_button_get_active (g_ptr_array_index (checks, 3)));
    g_assert_nonnull (strstr (gtk_check_button_get_label (g_ptr_array_index (checks, 0)), "exact duplicate"));
    g_assert_nonnull (strstr (gtk_check_button_get_label (g_ptr_array_index (checks, 1)), "different secret"));

    GtkWidget *commit = find_widget (GTK_WIDGET (dialog), GTK_TYPE_BUTTON, "Import Selected");
    g_assert_nonnull (commit);
    g_signal_emit_by_name (commit, "clicked");
    settle ();
    g_assert_cmpuint (summary.added, ==, 2);
    g_assert_cmpuint (summary.skipped_duplicates, ==, 2);
    g_assert_cmpuint (json_array_size (fixture.db->in_memory_json_data), ==, 3);
    g_object_unref (dialog);
    review_fixture_clear (&fixture);
}

static void
test_undo_waits_for_database_operation (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    (void) entry;
    json_array_append_new (fixture.db->in_memory_json_data,
        build_json_obj ("TOTP", "bob", "Example", REVIEW_SECRET, 6, "SHA1", 30, 0, NULL));
    GError *err = NULL;
    update_db (fixture.db, &err);
    g_assert_no_error (err);

    gtk_widget_activate_action (GTK_WIDGET (win), "win.delete-token", NULL);
    settle ();
    AdwDialog *dialog = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win));
    GtkWidget *button = find_widget (GTK_WIDGET (dialog), GTK_TYPE_BUTTON, "Delete");
    g_assert_nonnull (button);
    g_signal_emit_by_name (button, "clicked");
    settle ();
    g_assert_cmpuint (json_array_size (fixture.db->in_memory_json_data), ==, 1);

    /* Undo during an encryption job cannot write, but must not be lost: the
     * click dismisses its toast, so a fresh Undo has to be offered. */
    g_assert_true (otpclient_application_begin_database_operation (app, fixture.db));
    gpointer before = win->undo_actions->data;
    otpclient_window_undo_latest_delete (win);
    g_assert_cmpuint (json_array_size (fixture.db->in_memory_json_data), ==, 1);
    g_assert_true (win->undo_actions->data != before);
    otpclient_application_end_database_operation (app, fixture.db);

    otpclient_window_undo_latest_delete (win);
    g_assert_cmpuint (json_array_size (fixture.db->in_memory_json_data), ==, 2);
    g_assert_cmpstr (json_string_value (json_object_get (
        json_array_get (fixture.db->in_memory_json_data, 0), "label")), ==, "alice");
    g_assert_cmpstr (json_string_value (json_object_get (
        json_array_get (fixture.db->in_memory_json_data, 0), "secret")), ==, REVIEW_SECRET);
    review_fixture_clear (&fixture);
}

static void
test_bulk_group_assignment (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    (void) entry;
    json_object_set_new (json_array_get (fixture.db->in_memory_json_data, 0),
                         "group", json_string ("Personal"));
    json_array_append_new (fixture.db->in_memory_json_data,
        build_json_obj ("TOTP", "bob", "Example", REVIEW_SECRET, 6,
                        "SHA1", 30, 0, "Work"));
    GError *err = NULL;
    update_db (fixture.db, &err);
    g_assert_no_error (err);
    otpclient_window_rebuild_groups (win);

    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (win->bulk_mode_button), TRUE);
    settle ();
    g_autoptr (GPtrArray) checks = g_ptr_array_new ();
    collect_check_buttons (GTK_WIDGET (win), checks);
    g_assert_cmpuint (checks->len, >, 0);
    gtk_check_button_set_active (g_ptr_array_index (checks, 0), TRUE);
    g_assert_cmpuint (g_hash_table_size (win->bulk_selected_entries), ==, 1);

    guint group_count = g_list_model_get_n_items (G_LIST_MODEL (win->bulk_group_model));
    guint work_index = GTK_INVALID_LIST_POSITION;
    for (guint i = 0; i < group_count; i++) {
        if (g_strcmp0 (gtk_string_list_get_string (win->bulk_group_model, i), "Work") == 0)
            work_index = i;
    }
    g_assert_cmpuint (work_index, !=, GTK_INVALID_LIST_POSITION);
    gtk_drop_down_set_selected (GTK_DROP_DOWN (win->bulk_group_dropdown), work_index);
    g_assert_true (gtk_widget_activate_action (GTK_WIDGET (win), "win.bulk-set-group", NULL));
    g_assert_cmpstr (json_string_value (json_object_get (
        json_array_get (fixture.db->in_memory_json_data, 0), "group")), ==, "Work");
    review_fixture_clear (&fixture);
}

static void
test_dialog_lock_and_export_validation (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    g_autoptr (GPtrArray) dialogs = g_ptr_array_new_with_free_func (g_object_unref);
    AdwDialog *all[] = {
        ADW_DIALOG (qr_display_dialog_new ("otpauth://totp/alice?secret=" REVIEW_SECRET, "QR")),
        ADW_DIALOG (manual_add_dialog_new (fixture.db, NULL, NULL)),
        ADW_DIALOG (edit_token_dialog_new (json_array_get (fixture.db->in_memory_json_data, 0), 0, fixture.db, NULL, NULL)),
        ADW_DIALOG (import_dialog_new (fixture.db, GTK_WIDGET (win), NULL, NULL)),
        ADW_DIALOG (export_dialog_new (fixture.db, GTK_WIDGET (win))),
        ADW_DIALOG (password_dialog_new (PASSWORD_MODE_NEW, NULL, NULL)),
    };
    for (guint i = 0; i < G_N_ELEMENTS (all); i++) {
        g_ptr_array_add (dialogs, g_object_ref_sink (all[i]));
        adw_dialog_present (all[i], GTK_WIDGET (win));
    }
    GtkWidget *export = GTK_WIDGET (all[4]);
    GtkWidget *format = find_widget (export, ADW_TYPE_COMBO_ROW, "Format");
    GtkWidget *password = find_widget (export, ADW_TYPE_PASSWORD_ENTRY_ROW, "Encryption Password");
    GtkWidget *confirm = find_widget (export, ADW_TYPE_PASSWORD_ENTRY_ROW, "Confirm Password");
    GtkWidget *button = find_widget (export, GTK_TYPE_BUTTON, "Export");
    adw_combo_row_set_selected (ADW_COMBO_ROW (format), 2);
    g_assert_false (gtk_widget_get_sensitive (button));
    gtk_editable_set_text (GTK_EDITABLE (password), "export-password");
    gtk_editable_set_text (GTK_EDITABLE (confirm), "different");
    g_assert_false (gtk_widget_get_sensitive (button));
    gtk_editable_set_text (GTK_EDITABLE (confirm), "export-password");
    g_assert_true (gtk_widget_get_sensitive (button));
    GtkWidget *secret = find_widget (GTK_WIDGET (all[1]), ADW_TYPE_ENTRY_ROW, "Secret (Base32) or otpauth:// URI");
    g_assert_nonnull (secret);
    gtk_editable_set_text (GTK_EDITABLE (secret), REVIEW_SECRET);
    settle ();
    lock_app_lock (app);
    settle ();
    g_assert_true (otpclient_application_get_app_locked (app));
    g_assert_null (fixture.db->in_memory_json_data);
    for (guint i = 0; i < dialogs->len; i++) {
        AdwDialog *dialog = g_ptr_array_index (dialogs, i);
        g_assert_true (sensitive_dialog_is_closed (dialog));
        g_assert_true (g_cancellable_is_cancelled (sensitive_dialog_get_cancellable (dialog)));
        assert_cleared (GTK_WIDGET (dialog));
    }
    AdwDialog *unlock = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win));
    g_assert_true (PASSWORD_IS_DIALOG (unlock));
    adw_dialog_close (unlock);
    settle ();
    g_assert_null (adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win)));
    g_assert_true (otpclient_application_get_app_locked (app));
    review_fixture_clear (&fixture);
}

static GtkWidget *
find_label_containing (GtkWidget *parent, const gchar *needle)
{
    if (GTK_IS_LABEL (parent) &&
        strstr (gtk_label_get_text (GTK_LABEL (parent)), needle) != NULL)
        return parent;
    for (GtkWidget *child = gtk_widget_get_first_child (parent); child != NULL;
         child = gtk_widget_get_next_sibling (child)) {
        GtkWidget *found = find_label_containing (child, needle);
        if (found != NULL) return found;
    }
    return NULL;
}

static void
test_manual_add_reports_rejected_uri (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    (void) entry;
    AdwDialog *dialog = ADW_DIALOG (manual_add_dialog_new (fixture.db, NULL, NULL));
    adw_dialog_present (dialog, GTK_WIDGET (win));
    settle ();
    GtkWidget *secret = find_widget (GTK_WIDGET (dialog), ADW_TYPE_ENTRY_ROW, "Secret (Base32) or otpauth:// URI");
    g_assert_nonnull (secret);

    /* The URI is well formed but HOTP needs a counter: name that. */
    gtk_editable_set_text (GTK_EDITABLE (secret), "otpauth://hotp/alice?secret=" REVIEW_SECRET);
    GtkWidget *reason = find_label_containing (GTK_WIDGET (dialog), "missing its counter");
    g_assert_nonnull (reason);
    g_assert_true (gtk_widget_get_visible (reason));

    gtk_editable_set_text (GTK_EDITABLE (secret), "otpauth://hotp/alice?counter=3&secret=" REVIEW_SECRET);
    g_assert_false (gtk_widget_get_visible (reason));
    g_assert_cmpstr (gtk_editable_get_text (GTK_EDITABLE (secret)), ==, REVIEW_SECRET);
    adw_dialog_force_close (dialog);
    settle ();
    review_fixture_clear (&fixture);
}

static void
test_cross_database_sources_watched_before_load (void)
{
    ReviewFixture first, second;
    g_autoptr (OTPEntry) entry = attach_fixture (&first);
    (void) entry;
    review_fixture_init (&second);
    g_autoptr (GSettings) settings = gsettings_common_get_settings ();
    g_assert_nonnull (settings);
    GVariantBuilder builder;
    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ss)"));
    g_variant_builder_add (&builder, "(ss)", "First", first.path);
    g_variant_builder_add (&builder, "(ss)", "Second", second.path);
    g_settings_set_value (settings, "db-list", g_variant_builder_end (&builder));
    otpclient_application_set_use_secret_service (app, TRUE);

    gtk_editable_set_text (GTK_EDITABLE (win->search_entry), "alice");
    g_signal_emit_by_name (win->search_entry, "search-changed");

    /* The worker has not finished, yet the other database is already watched,
     * so a write to it during the load cannot be missed. The active one never
     * is: the app's own saves must not invalidate its own cache. */
    g_assert_true (win->cross_db_loading);
    g_assert_nonnull (win->cross_db_monitors);
    g_assert_true (g_hash_table_contains (win->cross_db_monitors, second.path));
    g_assert_false (g_hash_table_contains (win->cross_db_monitors, first.path));

    /* Locking starts a new generation and drops every monitor, so a load that
     * was in flight cannot be mistaken for the one after the next unlock. */
    guint64 generation = win->cross_db_generation;
    lock_app_lock (app);
    settle ();
    g_assert_cmpuint (win->cross_db_generation, >, generation);
    g_assert_false (win->cross_db_loading);
    g_assert_cmpuint (g_hash_table_size (win->cross_db_monitors), ==, 0);
    AdwDialog *unlock = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win));
    if (unlock != NULL) adw_dialog_force_close (unlock);

    otpclient_application_set_use_secret_service (app, FALSE);
    g_settings_reset (settings, "db-list");
    review_fixture_clear (&second);
    review_fixture_clear (&first);
}

static void
test_cross_database_search_requires_unlock (gconstpointer data)
{
    gboolean lock_during_load = GPOINTER_TO_UINT (data) != 0;
    ReviewFixture first, second;
    g_autoptr (OTPEntry) entry = attach_fixture (&first);
    (void) entry;
    review_fixture_init (&second);
    json_array_clear (second.db->in_memory_json_data);
    json_array_append_new (second.db->in_memory_json_data,
        build_json_obj ("TOTP", "alice", "Example", REVIEW_SECRET,
                        8, "SHA1", 30, 0, NULL));
    GError *err = NULL;
    update_db (second.db, &err);
    g_assert_no_error (err);
    g_autoptr (GSettings) settings = gsettings_common_get_settings ();
    GVariantBuilder builder;
    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(ss)"));
    g_variant_builder_add (&builder, "(ss)", "First", first.path);
    g_variant_builder_add (&builder, "(ss)", "Second", second.path);
    g_settings_set_value (settings, "db-list", g_variant_builder_end (&builder));
    provide_lookup_password = TRUE;
    otpclient_application_set_use_secret_service (app, TRUE);

    /* First prove the fake keyring can load real encrypted token data. */
    gtk_editable_set_text (GTK_EDITABLE (win->search_entry), "alice");
    g_signal_emit_by_name (win->search_entry, "search-changed");
    gint64 deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
    while (win->cross_db_loading && g_get_monotonic_time () < deadline)
        settle ();
    g_assert_true (win->cross_db_loaded);
    g_assert_cmpuint (g_list_model_get_n_items (
        G_LIST_MODEL (win->cross_db_store)), ==, 1);
    g_autoptr (OTPEntry) loaded = g_list_model_get_item (
        G_LIST_MODEL (win->cross_db_store), 0);
    otp_entry_test_update_otp_at (loaded, 59);
    g_assert_cmpstr (otp_entry_get_otp_value (loaded), ==, "94287082");
    g_clear_object (&loaded);
    otpclient_window_invalidate_cross_db (win);
    gtk_editable_set_text (GTK_EDITABLE (win->search_entry), "");
    g_signal_emit_by_name (win->search_entry, "search-changed");

    if (!lock_during_load)
        lock_app_enter_locked_state (app);
    gint lookups_before = g_atomic_int_get (&password_lookup_calls);
    gtk_widget_activate_action (GTK_WIDGET (win), "window.search", NULL);
    gtk_editable_set_text (GTK_EDITABLE (win->search_entry), "alice");
    g_signal_emit_by_name (win->search_entry, "search-changed");
    if (lock_during_load) {
        g_assert_true (win->cross_db_loading);
        /* Completion must check the state independently of the cleanup that
         * normally cancels the task and invalidates its generation. */
        otpclient_application_set_app_locked (app, TRUE);
    } else {
        g_assert_false (win->cross_db_loading);
    }
    deadline = g_get_monotonic_time () + 5 * G_USEC_PER_SEC;
    do {
        settle ();
    } while (win->cross_db_loading && g_get_monotonic_time () < deadline);
    g_assert_false (win->cross_db_loading);
    g_assert_false (win->cross_db_loaded);
    g_assert_cmpuint (g_list_model_get_n_items (
        G_LIST_MODEL (win->cross_db_store)), ==, 0);
    if (!lock_during_load) {
        g_assert_cmpint (g_atomic_int_get (&password_lookup_calls), ==,
                         lookups_before);
        g_assert_cmpuint (g_hash_table_size (win->cross_db_monitors), ==, 0);
    }
    otpclient_window_secure_lock_cleanup (win);
    otpclient_application_set_use_secret_service (app, FALSE);
    provide_lookup_password = FALSE;
    g_settings_reset (settings, "db-list");
    review_fixture_clear (&second);
    review_fixture_clear (&first);
}

static void
test_cross_database_hotp_opens_database (void)
{
    ReviewFixture first, second;
    g_autoptr (OTPEntry) entry = attach_fixture (&first);
    review_fixture_init (&second);
    otp_entry_set_db_name (entry, "Other database");
    otp_entry_set_db_path (entry, second.path);
    g_assert_true (gtk_widget_activate_action (GTK_WIDGET (win), "win.activate-token", NULL));
    DatabaseData *active = otpclient_application_get_db_data (app);
    g_assert_nonnull (active);
    g_assert_cmpstr (active->db_path, ==, second.path);
    g_assert_cmpuint (otp_entry_get_counter (entry), ==, 0);
    g_assert_null (otp_entry_get_otp_value (entry));
    g_assert_cmpint (json_integer_value (json_object_get (json_array_get (second.db->in_memory_json_data, 0), "counter")), ==, 0);
    AdwDialog *unlock = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win));
    if (unlock != NULL) adw_dialog_force_close (unlock);
    review_fixture_clear (&second);
    review_fixture_clear (&first);
}

/* A double-click on a row is turned by GTK into list.activate-item, which the
 * column view re-emits as ::activate. Xvfb gives us no portable way to
 * synthesize the two presses, so emit what GTK would and check that the row
 * the position points at is the one that gets copied and revealed. */
static void
test_double_click_activates_row (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    otpclient_application_set_hide_otps (app, TRUE);
    g_assert_cmpuint (otp_entry_get_counter (entry), ==, 0);
    g_assert_false (otp_entry_get_revealed (entry));
    g_signal_emit_by_name (win->otp_list, "activate", 0u);
    g_assert_cmpstr (otp_entry_get_otp_value (entry), ==, "755224");
    g_assert_cmpuint (otp_entry_get_counter (entry), ==, 1);
    g_assert_true (otp_entry_get_revealed (entry));
    g_assert_nonnull (win->clipboard_content);
    otpclient_window_clear_clipboard_now (win);
    review_fixture_clear (&fixture);
}

/* Count the rows currently showing a countdown. The cell is the level bar's
 * parent box: which of its two children is on depends on the "show validity
 * seconds" preference, but the box itself is hidden outright when there is no
 * countdown to show. Counting rather than finding the first one keeps the
 * assertion honest when the column view is holding a recycled, unbound row. */
static guint
visible_validity_cells (GtkWidget *parent)
{
    guint n = 0;
    if (GTK_IS_LEVEL_BAR (parent)) {
        GtkWidget *box = gtk_widget_get_parent (parent);
        if (box != NULL && gtk_widget_get_visible (box))
            n++;
    }
    for (GtkWidget *child = gtk_widget_get_first_child (parent); child != NULL;
         child = gtk_widget_get_next_sibling (child))
        n += visible_validity_cells (child);
    return n;
}

/* The countdown belongs to the code it counts down, not to the cursor.
 * Selection used to gate it, back when selecting a row was what revealed a
 * code, and it no longer does either: a visible TOTP keeps its bar with
 * nothing selected, a masked one has none, and HOTP never gets one because its
 * code lasts until consumed rather than until the clock rolls over. */
static void
test_validity_follows_the_code (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) hotp = attach_fixture (&fixture);

    /* Codes visible for every row, and row 0 (the HOTP) still selected. */
    otpclient_application_set_hide_otps (app, FALSE);
    settle ();
    g_assert_cmpuint (visible_validity_cells (GTK_WIDGET (win)), ==, 0);

    g_list_store_remove_all (win->otp_store);
    g_autoptr (OTPEntry) totp = otp_entry_new ("bob", "Example", NULL, "TOTP", 30, 0,
                                               "SHA1", 6, REVIEW_SECRET);
    otp_entry_update_otp (totp);
    g_list_store_append (win->otp_store, totp);
    gtk_single_selection_set_selected (win->otp_selection, GTK_INVALID_LIST_POSITION);
    settle ();
    g_assert_cmpuint (visible_validity_cells (GTK_WIDGET (win)), ==, 1);

    /* Masking the code takes the countdown with it, and revealing the row
     * brings it back, both without the selection changing. */
    otpclient_application_set_hide_otps (app, TRUE);
    settle ();
    g_assert_cmpuint (visible_validity_cells (GTK_WIDGET (win)), ==, 0);

    otp_entry_set_revealed (totp, TRUE);
    settle ();
    g_assert_cmpuint (visible_validity_cells (GTK_WIDGET (win)), ==, 1);

    otp_entry_set_revealed (totp, FALSE);
    review_fixture_clear (&fixture);
}

/* Both ends of the "group:" prefix sync, which walks the dropdown's model
 * looking for the group the user is typing. The model reads
 * ["All", <groups...>, "Ungrouped"], so the walk has to skip a sentinel at each
 * end without ever assuming there are two of them: while the database is locked
 * the model is empty, not sentinel-only, and the old `i < n_items - 1` bound
 * underflowed to 4294967295 there and read past the end for as long as the user
 * was willing to wait, two criticals an iteration. The search box is reachable
 * on the locked page: it sits outside content_stack, and #467 deliberately
 * leaves the toolbar live after Escape. Criticals are fatal in this binary, so
 * reaching the end of this function is half the assertion. */
static void
test_group_search_bounds (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    json_array_append_new (fixture.db->in_memory_json_data,
        build_json_obj ("TOTP", "bob", "Example", REVIEW_SECRET, 6, "SHA1", 30, 0, "Work"));
    otpclient_window_rebuild_groups (win);
    settle ();
    /* All, Work, Ungrouped: the only real group is the one between them. */
    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (win->group_list_model)), ==, 3);

    gtk_search_bar_set_search_mode (GTK_SEARCH_BAR (win->search_bar), TRUE);
    gtk_editable_set_text (GTK_EDITABLE (win->search_entry), "group:work");
    /* GtkSearchEntry delays "search-changed"; emit what it would emit. */
    g_signal_emit_by_name (win->search_entry, "search-changed");
    settle ();
    g_assert_cmpuint (gtk_drop_down_get_selected (GTK_DROP_DOWN (win->group_dropdown)), ==, 1);

    gtk_editable_set_text (GTK_EDITABLE (win->search_entry), "");
    g_signal_emit_by_name (win->search_entry, "search-changed");
    settle ();

    lock_app_lock (app);
    settle ();
    AdwDialog *unlock = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win));
    if (unlock != NULL) adw_dialog_force_close (unlock);
    settle ();
    g_assert_true (otpclient_application_get_app_locked (app));
    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (win->group_list_model)), ==, 0);

    /* A bare "group:" is enough: search_group becomes a non-NULL "". */
    gtk_editable_set_text (GTK_EDITABLE (win->search_entry), "group:");
    g_signal_emit_by_name (win->search_entry, "search-changed");
    settle ();

    gtk_editable_set_text (GTK_EDITABLE (win->search_entry), "");
    g_signal_emit_by_name (win->search_entry, "search-changed");
    gtk_search_bar_set_search_mode (GTK_SEARCH_BAR (win->search_bar), FALSE);
    settle ();
    review_fixture_clear (&fixture);
}

static void
test_group_search_preserves_all_matches (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    otpclient_application_set_use_secret_service (app, FALSE);
    json_object_set_new (json_array_get (fixture.db->in_memory_json_data, 0),
                         "group", json_string ("Work"));
    json_array_append_new (fixture.db->in_memory_json_data,
        build_json_obj ("TOTP", "bob", "Example", REVIEW_SECRET,
                        6, "SHA1", 30, 0, "Work Projects"));
    otpclient_window_database_modified (win);

    /* Replace an existing exact dropdown filter with a broader query. */
    gtk_drop_down_set_selected (GTK_DROP_DOWN (win->group_dropdown), 1);
    g_assert_cmpuint (g_list_model_get_n_items (
        G_LIST_MODEL (win->filter_model)), ==, 1);
    const gchar *queries[] = {
        "group:Work", "#work", "group:\"Work Projects\" bob", "group:missing"
    };
    const guint expected[] = { 2, 2, 1, 0 };
    const guint dropdown[] = { 0, 0, 2, 0 };
    for (guint i = 0; i < G_N_ELEMENTS (queries); i++) {
        gtk_editable_set_text (GTK_EDITABLE (win->search_entry), queries[i]);
        g_signal_emit_by_name (win->search_entry, "search-changed");
        g_assert_cmpuint (g_list_model_get_n_items (
            G_LIST_MODEL (win->filter_model)), ==, expected[i]);
        g_assert_cmpuint (gtk_drop_down_get_selected (
            GTK_DROP_DOWN (win->group_dropdown)), ==, dropdown[i]);
        g_assert_null (win->active_group_filter);
    }
    gtk_widget_activate_action (GTK_WIDGET (win), "win.clear-search", NULL);
    settle ();
    review_fixture_clear (&fixture);
}

static void
test_clear_filters_resets_group_only_filter (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    json_object_set_new (json_array_get (fixture.db->in_memory_json_data, 0),
                         "group", json_string ("Work"));
    otpclient_window_database_modified (win);
    gtk_widget_activate_action (GTK_WIDGET (win), "win.clear-search", NULL);

    /* All tokens are grouped, so Ungrouped displays the recovery button. */
    guint count = g_list_model_get_n_items (G_LIST_MODEL (win->group_list_model));
    gtk_drop_down_set_selected (GTK_DROP_DOWN (win->group_dropdown), count - 1);
    g_assert_cmpstr (gtk_editable_get_text (
        GTK_EDITABLE (win->search_entry)), ==, "");
    g_assert_cmpuint (g_list_model_get_n_items (
        G_LIST_MODEL (win->filter_model)), ==, 0);
    g_assert_cmpstr (gtk_stack_get_visible_child_name (
        GTK_STACK (win->content_stack)), ==, "no-search-results");
    gtk_widget_activate_action (GTK_WIDGET (win), "win.clear-search", NULL);
    g_assert_null (win->active_group_filter);
    g_assert_cmpuint (gtk_drop_down_get_selected (
        GTK_DROP_DOWN (win->group_dropdown)), ==, 0);
    g_assert_cmpuint (g_list_model_get_n_items (
        G_LIST_MODEL (win->filter_model)), ==, 1);
    g_assert_cmpstr (gtk_stack_get_visible_child_name (
        GTK_STACK (win->content_stack)), ==, "list");

    /* Clearing nonempty search text must also update the results immediately. */
    gtk_editable_set_text (GTK_EDITABLE (win->search_entry), "no such account");
    g_signal_emit_by_name (win->search_entry, "search-changed");
    g_assert_cmpuint (g_list_model_get_n_items (
        G_LIST_MODEL (win->filter_model)), ==, 0);
    gtk_widget_activate_action (GTK_WIDGET (win), "win.clear-search", NULL);
    g_assert_cmpuint (g_list_model_get_n_items (
        G_LIST_MODEL (win->filter_model)), ==, 1);
    settle ();
    review_fixture_clear (&fixture);
}

static GtkDropTarget *
find_drop_target (GtkWidget *widget)
{
    g_autoptr (GListModel) controllers = gtk_widget_observe_controllers (widget);
    guint n = g_list_model_get_n_items (controllers);
    for (guint i = 0; i < n; i++) {
        g_autoptr (GObject) c = g_list_model_get_item (controllers, i);
        if (GTK_IS_DROP_TARGET (c))
            return GTK_DROP_TARGET (c);
    }
    return NULL;
}

static void
test_encryption_operation_blocks_token_actions (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    GtkWidget *generate = find_widget (GTK_WIDGET (win), GTK_TYPE_BUTTON, "view-refresh-symbolic");
    g_assert_nonnull (generate);

    /* A response that was already pending must obey the guard too. */
    gtk_widget_activate_action (GTK_WIDGET (win), "win.delete-token", NULL);
    settle ();
    AdwDialog *confirm = adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win));
    g_assert_nonnull (confirm);
    GtkWidget *remove = find_widget (GTK_WIDGET (confirm), GTK_TYPE_BUTTON, "Delete");
    g_assert_nonnull (remove);
    g_assert_true (otpclient_application_begin_database_operation (app, fixture.db));
    g_signal_emit_by_name (remove, "clicked");
    settle ();
    g_signal_emit_by_name (generate, "clicked");
    gtk_widget_activate_action (GTK_WIDGET (win), "win.activate-token", NULL);
    g_assert_cmpuint (otp_entry_get_counter (entry), ==, 0);
    g_assert_cmpint (json_integer_value (json_object_get (
        json_array_get (fixture.db->in_memory_json_data, 0), "counter")), ==, 0);

    json_t *deleted = build_json_obj ("TOTP", "deleted", "Example", REVIEW_SECRET, 6, "SHA1", 30, 0, NULL);
    otpclient_window_undo_show_single (win, fixture.db, deleted, 1);
    json_decref (deleted);
    otpclient_window_undo_latest_delete (win);
    g_assert_cmpuint (json_array_size (fixture.db->in_memory_json_data), ==, 1);

    GtkDropTarget *drop = find_drop_target (win->otp_list);
    g_assert_nonnull (drop);
    GValue value = G_VALUE_INIT;
    g_value_init (&value, G_TYPE_UINT);
    g_value_set_uint (&value, 0);
    gboolean handled = TRUE;
    g_signal_emit_by_name (drop, "drop", &value, 0.0, 0.0, &handled);
    g_value_unset (&value);
    g_assert_false (handled);
    g_assert_true (json_equal (fixture.db->in_memory_json_data, fixture.db->committed_json_data));

    gtk_widget_activate_action (GTK_WIDGET (win), "win.manage-groups", NULL);
    g_action_group_activate_action (G_ACTION_GROUP (app), "kdf-settings", NULL);
    g_action_group_activate_action (G_ACTION_GROUP (app), "change-password", NULL);
    g_action_group_activate_action (G_ACTION_GROUP (app), "db-info", NULL);
    g_assert_null (adw_application_window_get_visible_dialog (ADW_APPLICATION_WINDOW (win)));

    otpclient_application_end_database_operation (app, fixture.db);
    g_signal_emit_by_name (generate, "clicked");
    g_assert_cmpuint (otp_entry_get_counter (entry), ==, 1);
    otpclient_window_undo_latest_delete (win);
    g_assert_cmpuint (json_array_size (fixture.db->in_memory_json_data), ==, 2);
    review_fixture_clear (&fixture);
}

static void
test_encryption_operation_blocks_pending_edit (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    EditTokenDialog *edit = edit_token_dialog_new (
        json_array_get (fixture.db->in_memory_json_data, 0), 0, fixture.db, NULL, NULL);
    g_object_ref_sink (edit);
    adw_dialog_present (ADW_DIALOG (edit), GTK_WIDGET (win));
    GtkWidget *label = find_widget (GTK_WIDGET (edit), ADW_TYPE_ENTRY_ROW, "Label");
    GtkWidget *save = find_widget (GTK_WIDGET (edit), GTK_TYPE_BUTTON, "Save");
    g_assert_nonnull (label);
    g_assert_nonnull (save);
    gtk_editable_set_text (GTK_EDITABLE (label), "changed");

    g_assert_true (otpclient_application_begin_database_operation (app, fixture.db));
    g_signal_emit_by_name (save, "clicked");
    g_assert_true (json_equal (fixture.db->in_memory_json_data, fixture.db->committed_json_data));
    g_assert_cmpstr (json_string_value (json_object_get (
        json_array_get (fixture.db->in_memory_json_data, 0), "label")), ==, "alice");
    otpclient_application_end_database_operation (app, fixture.db);
    g_signal_emit_by_name (save, "clicked");
    settle ();
    g_assert_cmpstr (json_string_value (json_object_get (
        json_array_get (fixture.db->in_memory_json_data, 0), "label")), ==, "changed");
    g_object_unref (edit);
    review_fixture_clear (&fixture);
}

static void
test_edit_dialog_counter_bound_is_exclusive (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    (void) entry;
    EditTokenDialog *edit = edit_token_dialog_new (
        json_array_get (fixture.db->in_memory_json_data, 0), 0, fixture.db, NULL, NULL);
    g_object_ref_sink (edit);
    adw_dialog_present (ADW_DIALOG (edit), GTK_WIDGET (win));
    GtkWidget *counter = find_widget (GTK_WIDGET (edit), ADW_TYPE_ENTRY_ROW, "Next unused counter");
    GtkWidget *save = find_widget (GTK_WIDGET (edit), GTK_TYPE_BUTTON, "Save");
    g_assert_nonnull (counter);
    g_assert_nonnull (save);

    /* OTP_HOTP_COUNTER_MAX is exclusive: 2^48 must be refused by the dialog
     * itself, not by the generic validation error after the write attempt. */
    gtk_editable_set_text (GTK_EDITABLE (counter), "281474976710656");
    g_signal_emit_by_name (save, "clicked");
    settle ();
    g_assert_cmpint (json_integer_value (json_object_get (
        json_array_get (fixture.db->in_memory_json_data, 0), "counter")), ==, 0);
    g_assert_nonnull (find_label_containing (GTK_WIDGET (edit), "supported range"));

    gtk_editable_set_text (GTK_EDITABLE (counter), "281474976710655");
    g_signal_emit_by_name (save, "clicked");
    settle ();
    g_assert_cmpint (json_integer_value (json_object_get (
        json_array_get (fixture.db->in_memory_json_data, 0), "counter")), ==, 281474976710655LL);
    g_object_unref (edit);
    review_fixture_clear (&fixture);
}

/* Row order in the store has to keep matching index order in the JSON, because
 * that is how every position-addressed action resolves its token. A failed save
 * after a drag-reorder used to break exactly that: update_db's failure path
 * restores the JSON to the pre-reorder snapshot while the store keeps the new
 * order, and the next Generate on an HOTP row then advances and persists a
 * different account's counter. */
static void
test_failed_reorder_restores_row_order (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    GError *err = NULL;
    json_array_append_new (fixture.db->in_memory_json_data,
        build_json_obj ("TOTP", "bob", "Example", REVIEW_SECRET, 6, "SHA1", 30, 0, NULL));
    update_db (fixture.db, &err);
    g_assert_no_error (err);

    /* Rebuild the rows from the JSON so the two start out in step. */
    g_autoptr (OTPEntry) bob = otp_entry_new ("bob", "Example", NULL, "TOTP", 30, 0,
                                              "SHA1", 6, REVIEW_SECRET);
    otp_entry_update_otp (bob);
    g_list_store_append (win->otp_store, bob);
    settle ();
    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (win->otp_store)), ==, 2);

    GtkDropTarget *drop = find_drop_target (win->otp_list);
    g_assert_nonnull (drop);

    /* Drop row 0 onto the lower half of row 1, which is the one arrangement
     * that asks for a real move rather than a no-op. */
    GtkWidget *button = find_widget (GTK_WIDGET (win), GTK_TYPE_BUTTON, "edit-copy-symbolic");
    g_assert_nonnull (button);
    GtkWidget *row = button;
    while (row != NULL && gtk_widget_get_parent (gtk_widget_get_parent (row)) != win->otp_list)
        row = gtk_widget_get_parent (row);
    g_assert_nonnull (row);
    graphene_point_t pt, btn;
    g_assert_true (gtk_widget_compute_point (row, win->otp_list, &GRAPHENE_POINT_INIT (0, 0), &pt));
    g_assert_true (gtk_widget_compute_point (button, win->otp_list, &GRAPHENE_POINT_INIT (0, 0), &btn));
    /* The button is where the row's "otp-entry" data is reachable by a pick, and
     * the exact vertical midpoint counts as the lower half (y >= midpoint), so
     * one point serves for both halves of what on_drop needs. */
    double drop_x = btn.x + gtk_widget_get_width (button) / 2.0;
    double drop_y = pt.y + gtk_widget_get_height (row) / 2.0;

    db_test_set_fail_atomic_write (TRUE);
    GValue value = G_VALUE_INIT;
    g_value_init (&value, G_TYPE_UINT);
    g_value_set_uint (&value, 0);
    gboolean handled = FALSE;
    g_signal_emit_by_name (drop, "drop", &value, drop_x, drop_y, &handled);
    g_value_unset (&value);
    db_test_set_fail_atomic_write (FALSE);
    g_assert_true (handled);
    settle ();

    /* The write failed, so the JSON is back in its original order and the rows
     * have to say the same thing. */
    guint n = g_list_model_get_n_items (G_LIST_MODEL (win->otp_store));
    g_assert_cmpuint (n, ==, json_array_size (fixture.db->in_memory_json_data));
    for (guint i = 0; i < n; i++) {
        g_autoptr (OTPEntry) row_entry = g_list_model_get_item (G_LIST_MODEL (win->otp_store), i);
        const gchar *label = json_string_value (
            json_object_get (json_array_get (fixture.db->in_memory_json_data, i), "label"));
        g_assert_cmpstr (otp_entry_get_account (row_entry), ==, label);
    }
    g_assert_cmpstr (json_string_value (json_object_get (
        json_array_get (fixture.db->in_memory_json_data, 0), "label")), ==, "alice");
    review_fixture_clear (&fixture);
}

/* B8: the autostart key is new in 5.2.0 and defaults to off, so an entry that
 * is already there when a launch finds the key untouched cannot be ours. The
 * startup reassert used to read the default and delete the user's file. */
static void
test_autostart_adoption (void)
{
    g_autofree gchar *dir = g_build_filename (g_get_user_config_dir (), "autostart", NULL);
    g_autofree gchar *path = g_build_filename (dir, APPLICATION_ID ".desktop", NULL);
    g_assert_cmpint (g_mkdir_with_parents (dir, 0700), ==, 0);

    g_autoptr (GSettings) settings = g_settings_new ("com.github.paolostivanin.OTPClient");
    g_settings_reset (settings, "autostart");
    otpclient_application_reload_settings (app);
    g_assert_true (otpclient_application_autostart_key_is_default (app));

    g_assert_true (g_file_set_contents (path,
        "[Desktop Entry]\nType=Application\nName=OTPClient\nExec=otpclient\n", -1, NULL));

    g_assert_true (autostart_adopt_existing_entry (app));
    g_assert_true (otpclient_application_get_autostart (app));
    g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));

    /* The key has a user value now, so the entry is this application's from
     * here on and a later launch must not read it as somebody else's again. */
    g_assert_false (autostart_adopt_existing_entry (app));

    /* An entry that is switched off is a no, not something to turn on. The key
     * still stops being a default, so the question is asked only once. */
    g_settings_reset (settings, "autostart");
    otpclient_application_reload_settings (app);
    g_assert_true (g_file_set_contents (path,
        "[Desktop Entry]\nType=Application\nName=OTPClient\nExec=otpclient\n"
        "X-GNOME-Autostart-enabled=false\n", -1, NULL));
    g_assert_false (autostart_adopt_existing_entry (app));
    g_assert_false (otpclient_application_get_autostart (app));
    g_assert_false (otpclient_application_autostart_key_is_default (app));

    /* No entry at all is the ordinary first launch: nothing to adopt, and the
     * key is left alone so a file that appears later is still adoptable. */
    g_settings_reset (settings, "autostart");
    otpclient_application_reload_settings (app);
    g_assert_cmpint (g_unlink (path), ==, 0);
    g_assert_false (autostart_adopt_existing_entry (app));
    g_assert_true (otpclient_application_autostart_key_is_default (app));
}

/* set_db_data() is the common replacement boundary used by the open/new-DB
 * flows that bypass switch_to_db(). It must invalidate pending async work
 * (chooser/import contexts) even when the path is unchanged, so a stale
 * context cannot be applied to the replacement database. */
static void
test_db_replacement_bumps_generation (void)
{
    guint before = otpclient_application_get_lock_generation (app);
    otpclient_application_set_db_data (app, NULL);
    g_assert_cmpuint (otpclient_application_get_lock_generation (app), >, before);

    /* Same-path replacement must also bump: a stale context keyed only to the
     * path would otherwise survive. */
    before = otpclient_application_get_lock_generation (app);
    DatabaseData *replacement = database_data_new ("/tmp/otpclient-generation.enc",
                                                   DEFAULT_MEMLOCK_VALUE);
    g_assert_true (otpclient_application_set_db_data (app, replacement));
    g_assert_cmpuint (otpclient_application_get_lock_generation (app), >, before);

    /* H1 defense in depth: a worker owns replacement while unlocking, so the
     * boundary must reject another database without changing ownership,
     * generation, or the active pointer. */
    DatabaseData *refused = database_data_new ("/tmp/otpclient-refused.enc",
                                               DEFAULT_MEMLOCK_VALUE);
    before = otpclient_application_get_lock_generation (app);
    otpclient_application_test_set_unlocking (app, TRUE);
    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING,
                           "*Refusing to replace the active database*");
    g_assert_false (otpclient_application_set_db_data (app, refused));
    g_test_assert_expected_messages ();
    g_assert_true (otpclient_application_get_db_data (app) == replacement);
    g_assert_cmpuint (otpclient_application_get_lock_generation (app), ==, before);
    otpclient_application_test_set_unlocking (app, FALSE);
    database_data_free (refused);

    g_assert_true (otpclient_application_set_db_data (app, NULL));
}

static void
test_encryption_operation_guards_database_lifetime (void)
{
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    DatabaseData *active = otpclient_application_get_db_data (app);
    g_assert_true (otpclient_application_begin_database_operation (app, active));
    g_assert_true (otpclient_application_is_database_operation_in_progress (app));

    DatabaseData *replacement = database_data_new ("/tmp/otpclient-operation.enc",
                                                   DEFAULT_MEMLOCK_VALUE);
    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING,
                           "*Refusing to replace the active database*");
    g_assert_false (otpclient_application_set_db_data (app, replacement));
    g_test_assert_expected_messages ();
    g_assert_true (otpclient_application_get_db_data (app) == active);
    database_data_free (replacement);

    /* Auto-Lock can invalidate async UI work immediately, while the worker
     * still owns the database. Secret clearing waits until the worker exits. */
    otpclient_application_purge_secrets (app);
    g_assert_nonnull (active->key);
    g_assert_nonnull (active->in_memory_json_data);
    otpclient_application_end_database_operation (app, active);
    g_assert_false (otpclient_application_is_database_operation_in_progress (app));
    g_assert_null (active->key);
    g_assert_null (active->in_memory_json_data);
    otpclient_application_set_app_locked (app, FALSE);
    review_fixture_clear (&fixture);
}

static void
start_password_change (void)
{
    g_action_group_activate_action (G_ACTION_GROUP (app), "change-password", NULL);
    AdwDialog *dialog = adw_application_window_get_visible_dialog (
        ADW_APPLICATION_WINDOW (win));
    g_assert_nonnull (dialog);
    const gchar *titles[] = {
        "Current Password", "New Password", "Confirm Password"
    };
    const gchar *passwords[] = {
        "test-password", "new-password", "new-password"
    };
    for (guint i = 0; i < G_N_ELEMENTS (titles); i++) {
        GtkWidget *row = find_widget (GTK_WIDGET (dialog),
                                      ADW_TYPE_PASSWORD_ENTRY_ROW, titles[i]);
        g_assert_nonnull (row);
        gtk_editable_set_text (GTK_EDITABLE (row), passwords[i]);
    }
    GtkWidget *button = find_widget (GTK_WIDGET (dialog),
                                     GTK_TYPE_BUTTON, "Change Password");
    g_assert_nonnull (button);
    g_signal_emit_by_name (button, "clicked");
    g_assert_true (otpclient_application_is_database_operation_in_progress (app));
}

static void
test_password_change_keyring (gconstpointer data)
{
    guint flags = GPOINTER_TO_UINT (data);
    gboolean lock = (flags & 1) != 0;
    gboolean fail = (flags & 2) != 0;
    gboolean disable_keyring = (flags & 4) != 0;
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    otpclient_application_set_use_secret_service (app, TRUE);
    password_store_calls = 0;
    g_clear_pointer (&stored_password, sensitive_secure_free);
    g_clear_pointer (&stored_password_path, g_free);
    db_test_set_fail_atomic_write (fail);
    start_password_change ();
    /* Lock before dispatching the worker completion, even on fast machines. */
    if (lock)
        lock_app_enter_locked_state (app);
    if (disable_keyring)
        otpclient_application_set_use_secret_service (app, FALSE);
    gint64 deadline = g_get_monotonic_time () + 10 * G_USEC_PER_SEC;
    while (otpclient_application_is_database_operation_in_progress (app) &&
           g_get_monotonic_time () < deadline)
        settle ();
    g_assert_false (otpclient_application_is_database_operation_in_progress (app));
    db_test_set_fail_atomic_write (FALSE);
    settle ();

    DatabaseData *reader = database_data_new (fixture.path, DEFAULT_MEMLOCK_VALUE);
    reader->key = secure_strdup (fail ? "test-password" : "new-password");
    GError *err = NULL;
    load_db (reader, &err);
    g_assert_no_error (err);
    g_assert_cmpuint (json_array_size (reader->in_memory_json_data), ==, 1);
    database_data_free (reader);

    guint expected_stores = !fail && !disable_keyring ? 1 : 0;
    g_assert_cmpuint (password_store_calls, ==, expected_stores);
    if (expected_stores != 0) {
        g_assert_cmpstr (stored_password, ==, "new-password");
        g_assert_cmpstr (stored_password_path, ==, fixture.path);
    }
    if (lock) {
        g_assert_true (otpclient_application_get_app_locked (app));
        g_assert_null (fixture.db->key);
        g_assert_null (fixture.db->in_memory_json_data);
        g_assert_false (gtk_widget_get_sensitive (win->otp_list));
        g_assert_null (adw_application_window_get_visible_dialog (
            ADW_APPLICATION_WINDOW (win)));
    }
    otpclient_application_set_use_secret_service (app, FALSE);
    g_clear_pointer (&stored_password, sensitive_secure_free);
    g_clear_pointer (&stored_password_path, g_free);
    review_fixture_clear (&fixture);
}

static gboolean
start_password_change_and_quit (gpointer data)
{
    (void) data;
    start_password_change ();
    /* No main-context iteration can dispatch the worker's completion yet. */
    g_action_group_activate_action (G_ACTION_GROUP (app), "quit", NULL);
    return G_SOURCE_REMOVE;
}

static gboolean
complete_delayed_password_store (gpointer data)
{
    (void) data;
    if (fail_password_store)
        g_task_return_new_error (pending_password_store, G_IO_ERROR,
                                 G_IO_ERROR_FAILED, "Test keyring failure");
    else
        g_task_return_boolean (pending_password_store, TRUE);
    g_clear_object (&pending_password_store);
    return G_SOURCE_REMOVE;
}

static gboolean
quit_during_password_store (gpointer data)
{
    (void) data;
    if (pending_password_store == NULL)
        return G_SOURCE_CONTINUE;
    /* A second request must also wait once only the keyring write remains. */
    g_assert_false (otpclient_application_is_database_operation_in_progress (app));
    otpclient_application_request_quit (app);
    g_idle_add (complete_delayed_password_store, NULL);
    return G_SOURCE_REMOVE;
}

static void
test_quit_waits_for_password_change (gconstpointer data)
{
    /* Quit ends GApplication's run loop permanently. Give each outcome its
     * own process while keeping the database and application paths real. */
    if (!g_test_subprocess ()) {
        g_test_trap_subprocess (NULL, 15 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_passed ();
        return;
    }
    guint flags = GPOINTER_TO_UINT (data);
    gboolean write_fails = (flags & 1) != 0;
    gboolean use_keyring = (flags & 4) == 0;
    fail_password_store = (flags & 2) != 0;
    ReviewFixture fixture;
    g_autoptr (OTPEntry) entry = attach_fixture (&fixture);
    (void) entry;
    otpclient_application_set_use_secret_service (app, use_keyring);
    delay_password_store = TRUE;
    password_store_calls = password_store_completions = 0;
    db_test_set_fail_atomic_write (write_fails);
    g_autoptr (GSettings) settings = gsettings_common_get_settings ();
    g_settings_set_string (settings, "last-seen-version", PROJECT_VER);
    g_idle_add (start_password_change_and_quit, NULL);
    guint store_source = g_timeout_add (1, quit_during_password_store, NULL);
    g_assert_cmpint (g_application_run (G_APPLICATION (app), 0, NULL), ==, 0);
    if (write_fails || !use_keyring)
        g_source_remove (store_source);
    db_test_set_fail_atomic_write (FALSE);

    g_assert_false (otpclient_application_is_database_operation_in_progress (app));
    g_assert_null (pending_password_store);
    guint expected_stores = !write_fails && use_keyring ? 1 : 0;
    g_assert_cmpuint (password_store_calls, ==, expected_stores);
    g_assert_cmpuint (password_store_completions, ==, expected_stores);
    if (expected_stores != 0) {
        g_assert_cmpstr (stored_password, ==, "new-password");
        g_assert_cmpstr (stored_password_path, ==, fixture.path);
    }
    DatabaseData *reader = database_data_new (fixture.path, DEFAULT_MEMLOCK_VALUE);
    reader->key = secure_strdup (write_fails ? "test-password" : "new-password");
    GError *err = NULL;
    load_db (reader, &err);
    g_assert_no_error (err);
    g_assert_cmpuint (json_array_size (reader->in_memory_json_data), ==, 1);
    database_data_free (reader);
    review_fixture_clear (&fixture);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    gint32 memory = 0;
    set_memlock_value (&memory);
    gchar *error = init_libs (memory);
    g_assert_null (error);
    /* Desktop services are deliberately absent from the private test bus. */
    g_log_set_always_fatal (G_LOG_LEVEL_ERROR | G_LOG_LEVEL_CRITICAL);
    app = otpclient_application_new ();
    g_application_set_flags (G_APPLICATION (app), G_APPLICATION_NON_UNIQUE);
    GError *err = NULL;
    g_assert_true (g_application_register (G_APPLICATION (app), NULL, &err));
    g_assert_no_error (err);
    g_object_set (gtk_settings_get_default (), "gtk-enable-animations", FALSE, NULL);
    win = OTPCLIENT_WINDOW (otpclient_application_get_window (app));
    otpclient_application_set_disable_notifications (app, TRUE);
    g_test_add_func ("/gui-flows/hotp-and-clipboard", test_hotp_and_clipboard);
    g_test_add_func ("/gui-flows/quarantine-review-delete", test_quarantine_review_can_delete_preserved_token);
    g_test_add_func ("/gui-flows/quarantine-review-repair", test_quarantine_review_repairs_and_validates_token);
    g_test_add_func ("/gui-flows/quarantine-repair-invalid-fields", test_quarantine_repair_reports_invalid_fields);
    g_test_add_func ("/gui-flows/quarantine-repair-normalizes-case", test_quarantine_repair_normalizes_case);
    g_test_add_func ("/gui-flows/manage-groups-rename", test_manage_groups_renames_all_matching_tokens);
    g_test_add_func ("/gui-flows/bulk-delete-undo", test_bulk_delete_restores_selected_tokens);
    g_test_add_func ("/gui-flows/single-delete-undo-write-failures", test_single_delete_and_undo_write_failures);
    g_test_add_func ("/gui-flows/undo-waits-for-database-operation", test_undo_waits_for_database_operation);
    g_test_add_func ("/gui-flows/bulk-group-assignment", test_bulk_group_assignment);
    g_test_add_func ("/gui-flows/bulk-selection-mode", test_bulk_selection_mode_refreshes_rows);
    g_test_add_func ("/gui-flows/bulk-selection-follows-filter", test_bulk_selection_follows_filter);
    g_test_add_func ("/gui-flows/import-preview", test_import_preview_duplicates_and_conflicts);
    g_test_add_func ("/gui-flows/dialog-lock-and-export-validation", test_dialog_lock_and_export_validation);
    g_test_add_func ("/gui-flows/manual-add-rejected-uri", test_manual_add_reports_rejected_uri);
    g_test_add_func ("/gui-flows/cross-database-watch", test_cross_database_sources_watched_before_load);
    g_test_add_data_func ("/gui-flows/cross-database-locked-search",
        GUINT_TO_POINTER (0), test_cross_database_search_requires_unlock);
    g_test_add_data_func ("/gui-flows/cross-database-lock-during-load",
        GUINT_TO_POINTER (1), test_cross_database_search_requires_unlock);
    g_test_add_func ("/gui-flows/cross-database-hotp", test_cross_database_hotp_opens_database);
    g_test_add_func ("/gui-flows/double-click-activates-row", test_double_click_activates_row);
    g_test_add_func ("/gui-flows/validity-follows-the-code", test_validity_follows_the_code);
    g_test_add_func ("/gui-flows/group-search-bounds", test_group_search_bounds);
    g_test_add_func ("/gui-flows/group-search-all-matches", test_group_search_preserves_all_matches);
    g_test_add_func ("/gui-flows/clear-group-filter", test_clear_filters_resets_group_only_filter);
    g_test_add_func ("/gui-flows/edit-counter-bound", test_edit_dialog_counter_bound_is_exclusive);
    g_test_add_func ("/gui-flows/failed-reorder-restores-row-order", test_failed_reorder_restores_row_order);
    g_test_add_func ("/gui-flows/autostart-adoption", test_autostart_adoption);
    g_test_add_func ("/gui-flows/db-replacement-generation", test_db_replacement_bumps_generation);
    g_test_add_func ("/gui-flows/encryption-operation-lifetime", test_encryption_operation_guards_database_lifetime);
    g_test_add_func ("/gui-flows/encryption-operation-token-actions", test_encryption_operation_blocks_token_actions);
    g_test_add_func ("/gui-flows/encryption-operation-pending-edit", test_encryption_operation_blocks_pending_edit);
    g_test_add_data_func ("/gui-flows/password-change-keyring", GUINT_TO_POINTER (0), test_password_change_keyring);
    g_test_add_data_func ("/gui-flows/password-change-keyring-locked", GUINT_TO_POINTER (1), test_password_change_keyring);
    g_test_add_data_func ("/gui-flows/password-change-keyring-failed", GUINT_TO_POINTER (3), test_password_change_keyring);
    g_test_add_data_func ("/gui-flows/password-change-keyring-disabled", GUINT_TO_POINTER (5), test_password_change_keyring);
    g_test_add_data_func ("/gui-flows/quit-password-change",
        GUINT_TO_POINTER (0), test_quit_waits_for_password_change);
    g_test_add_data_func ("/gui-flows/quit-password-change-write-failure",
        GUINT_TO_POINTER (1), test_quit_waits_for_password_change);
    g_test_add_data_func ("/gui-flows/quit-password-change-keyring-failure",
        GUINT_TO_POINTER (2), test_quit_waits_for_password_change);
    g_test_add_data_func ("/gui-flows/quit-password-change-no-keyring",
        GUINT_TO_POINTER (4), test_quit_waits_for_password_change);
    int result = g_test_run ();
    gtk_window_destroy (GTK_WINDOW (win));
    g_object_unref (app);
    g_clear_pointer (&stored_password, sensitive_secure_free);
    g_clear_pointer (&stored_password_path, g_free);
    return result;
}
