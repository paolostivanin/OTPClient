#include <string.h>
#include "review-fixture.h"
#include "otpclient-application.h"
#include "otpclient-window-private.h"
#include "otp-entry.h"
#include "lock-app.h"
#include "dialogs/qr-display-dialog.h"
#include "dialogs/password-dialog.h"
#include "dialogs/manual-add-dialog.h"
#include "dialogs/edit-token-dialog.h"
#include "dialogs/import-dialog.h"
#include "dialogs/export-dialog.h"
#include "dialogs/sensitive-dialog.h"

static OTPClientApplication *app;
static OTPClientWindow *win;

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
        const gchar *label = GTK_IS_BUTTON (parent) ? gtk_button_get_label (GTK_BUTTON (parent))
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
    GtkWidget *button = find_widget (GTK_WIDGET (win), GTK_TYPE_BUTTON, "Generate");
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
    g_test_add_func ("/gui-flows/dialog-lock-and-export-validation", test_dialog_lock_and_export_validation);
    g_test_add_func ("/gui-flows/cross-database-hotp", test_cross_database_hotp_opens_database);
    int result = g_test_run ();
    gtk_window_destroy (GTK_WINDOW (win));
    g_object_unref (app);
    return result;
}
