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
#include "autostart.h"
#include "version.h"

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
    g_test_add_func ("/gui-flows/double-click-activates-row", test_double_click_activates_row);
    g_test_add_func ("/gui-flows/validity-follows-the-code", test_validity_follows_the_code);
    g_test_add_func ("/gui-flows/group-search-bounds", test_group_search_bounds);
    g_test_add_func ("/gui-flows/failed-reorder-restores-row-order", test_failed_reorder_restores_row_order);
    g_test_add_func ("/gui-flows/autostart-adoption", test_autostart_adoption);
    g_test_add_func ("/gui-flows/db-replacement-generation", test_db_replacement_bumps_generation);
    int result = g_test_run ();
    gtk_window_destroy (GTK_WINDOW (win));
    g_object_unref (app);
    return result;
}
