/* Relocation is the "Locate..." recovery path: a database whose file moved, or
 * whose Flatpak document-portal id went stale after a reboot, is re-picked and
 * has to take over the sidebar row it already had rather than arrive as a
 * second row for the same database.
 *
 * The parts worth pinning down here are the ones with no UI in them: which row
 * ends up holding the new path, which rows are removed, and whether the path
 * change is announced. The last one is not decoration: the sidebar subtitle
 * only follows the path because notify::path fires, and DB_PROP_PATH used to be
 * CONSTRUCT_ONLY, so a change there is silent again and the sidebar goes back
 * to advertising a location the file has just been recovered from.
 *
 * The portal round trips around all this are not reachable from a test without
 * standing up a fake xdg-desktop-portal, so they are not attempted. */

#include <glib.h>
#include <gio/gio.h>
#include "gui-misc.h"
#include "database-sidebar.h"

static GListStore *
store_with (const gchar *first_name, const gchar *first_path, ...)
{
    GListStore *store = g_list_store_new (DATABASE_TYPE_ENTRY);

    va_list ap;
    va_start (ap, first_path);
    for (const gchar *name = first_name, *path = first_path; name != NULL; ) {
        g_autoptr (DatabaseEntry) entry = database_entry_new (name, path);
        g_list_store_append (store, entry);
        name = va_arg (ap, const gchar *);
        if (name == NULL)
            break;
        path = va_arg (ap, const gchar *);
    }
    va_end (ap);

    return store;
}

static DatabaseEntry *
nth (GListStore *store, guint i)
{
    return DATABASE_ENTRY (g_list_model_get_item (G_LIST_MODEL (store), i));
}

static void
assert_row (GListStore  *store,
            guint        i,
            const gchar *name,
            const gchar *path)
{
    g_autoptr (DatabaseEntry) entry = nth (store, i);
    g_assert_nonnull (entry);
    g_assert_cmpstr (database_entry_get_name (entry), ==, name);
    g_assert_cmpstr (database_entry_get_path (entry), ==, path);
}

/* The row keeps its identity: same position, same user-chosen name, and it
 * stops being marked missing because the file has just been opened. */
static void
test_replace_keeps_the_row (void)
{
    g_autoptr (GListStore) store = store_with ("Work", "/old/work.enc",
                                               "Home", "/home.enc",
                                               NULL);
    g_autoptr (DatabaseEntry) work = nth (store, 0);
    database_entry_set_missing (work, TRUE);

    g_assert_true (gui_misc_replace_db_path (store, "/old/work.enc", "/new/work.enc"));

    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (store)), ==, 2);
    assert_row (store, 0, "Work", "/new/work.enc");
    assert_row (store, 1, "Home", "/home.enc");
    g_assert_false (database_entry_get_missing (work));
}

/* An old path nobody lists is not something to guess about: the caller falls
 * back to appending a new row, so the store must come back untouched. */
static void
test_unknown_old_path_is_refused (void)
{
    g_autoptr (GListStore) store = store_with ("Work", "/work.enc", NULL);

    g_assert_false (gui_misc_replace_db_path (store, "/nowhere.enc", "/new.enc"));

    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (store)), ==, 1);
    assert_row (store, 0, "Work", "/work.enc");
}

/* Locating a database at a path that is already listed: the two rows are the
 * same database, so the one being relocated absorbs the other. Its name is the
 * one to keep, since the duplicate is what the user is recovering from. */
static void
test_duplicate_below_is_absorbed (void)
{
    g_autoptr (GListStore) store = store_with ("Stale copy", "/new/work.enc",
                                               "Home",       "/home.enc",
                                               "Work",       "/old/work.enc",
                                               NULL);

    g_assert_true (gui_misc_replace_db_path (store, "/old/work.enc", "/new/work.enc"));

    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (store)), ==, 2);
    assert_row (store, 0, "Home", "/home.enc");
    assert_row (store, 1, "Work", "/new/work.enc");
}

/* The same, with the duplicate after the relocated row. The dedupe pass walks
 * backwards over indices that shift as it removes, so which side the duplicate
 * falls on decides whether the captured index is still the right one. */
static void
test_duplicate_above_is_absorbed (void)
{
    g_autoptr (GListStore) store = store_with ("Work",       "/old/work.enc",
                                               "Home",       "/home.enc",
                                               "Stale copy", "/new/work.enc",
                                               NULL);

    g_assert_true (gui_misc_replace_db_path (store, "/old/work.enc", "/new/work.enc"));

    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (store)), ==, 2);
    assert_row (store, 0, "Work", "/new/work.enc");
    assert_row (store, 1, "Home", "/home.enc");
}

/* Two stale rows for the same database, one on each side. Both go, and the
 * relocated row is the survivor. */
static void
test_duplicates_on_both_sides (void)
{
    g_autoptr (GListStore) store = store_with ("Copy A", "/new/work.enc",
                                               "Work",   "/old/work.enc",
                                               "Copy B", "/new/work.enc",
                                               NULL);

    g_assert_true (gui_misc_replace_db_path (store, "/old/work.enc", "/new/work.enc"));

    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (store)), ==, 1);
    assert_row (store, 0, "Work", "/new/work.enc");
}

/* Re-picking the file it already points at. Nothing is a duplicate of itself,
 * so the row must survive; the loop skips old_index for exactly this reason. */
static void
test_relocating_onto_itself_keeps_the_row (void)
{
    g_autoptr (GListStore) store = store_with ("Work", "/work.enc", NULL);

    g_assert_true (gui_misc_replace_db_path (store, "/work.enc", "/work.enc"));

    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (store)), ==, 1);
    assert_row (store, 0, "Work", "/work.enc");
}

/* The absorbed row was the default. db-path is a path, and the path survives,
 * so the config still names this database; the star has to move with it or the
 * sidebar shows a default that no row admits to being. */
static void
test_absorbed_duplicate_hands_over_primary (void)
{
    g_autoptr (GListStore) store = store_with ("Stale copy", "/new/work.enc",
                                               "Work",       "/old/work.enc",
                                               NULL);
    g_autoptr (DatabaseEntry) stale = nth (store, 0);
    g_autoptr (DatabaseEntry) work = nth (store, 1);
    database_entry_set_primary (stale, TRUE);

    g_assert_true (gui_misc_replace_db_path (store, "/old/work.enc", "/new/work.enc"));

    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (store)), ==, 1);
    assert_row (store, 0, "Work", "/new/work.enc");
    g_assert_true (database_entry_get_primary (work));
}

/* The star is not handed out for free: absorbing an ordinary row leaves the
 * default where it was, on some third database. */
static void
test_absorbing_a_plain_duplicate_leaves_primary_alone (void)
{
    g_autoptr (GListStore) store = store_with ("Stale copy", "/new/work.enc",
                                               "Home",       "/home.enc",
                                               "Work",       "/old/work.enc",
                                               NULL);
    g_autoptr (DatabaseEntry) home = nth (store, 1);
    g_autoptr (DatabaseEntry) work = nth (store, 2);
    database_entry_set_primary (home, TRUE);

    g_assert_true (gui_misc_replace_db_path (store, "/old/work.enc", "/new/work.enc"));

    g_assert_cmpuint (g_list_model_get_n_items (G_LIST_MODEL (store)), ==, 2);
    g_assert_true (database_entry_get_primary (home));
    g_assert_false (database_entry_get_primary (work));
}

typedef struct {
    guint  count;
    gchar *last_seen;
} PathNotify;

static void
on_path_notify (GObject    *object,
                GParamSpec *pspec,
                gpointer    user_data)
{
    (void) pspec;
    PathNotify *seen = user_data;
    seen->count++;
    g_free (seen->last_seen);
    seen->last_seen = g_strdup (database_entry_get_path (DATABASE_ENTRY (object)));
}

/* What the sidebar subtitle hangs off. The notify has to carry the new value,
 * not merely fire, because the handler reads the property back. */
static void
test_path_change_is_announced (void)
{
    g_autoptr (GListStore) store = store_with ("Work", "/old/work.enc", NULL);
    g_autoptr (DatabaseEntry) work = nth (store, 0);

    PathNotify seen = { 0, NULL };
    g_signal_connect (work, "notify::path", G_CALLBACK (on_path_notify), &seen);

    g_assert_true (gui_misc_replace_db_path (store, "/old/work.enc", "/new/work.enc"));

    g_assert_cmpuint (seen.count, ==, 1);
    g_assert_cmpstr (seen.last_seen, ==, "/new/work.enc");
    g_free (seen.last_seen);
}

/* EXPLICIT_NOTIFY without the equality guard would repaint every row on every
 * save; the setter is meant to stay quiet when nothing moved. */
static void
test_unchanged_path_is_not_announced (void)
{
    g_autoptr (DatabaseEntry) entry = database_entry_new ("Work", "/work.enc");

    PathNotify seen = { 0, NULL };
    g_signal_connect (entry, "notify::path", G_CALLBACK (on_path_notify), &seen);

    database_entry_set_path (entry, "/work.enc");

    g_assert_cmpuint (seen.count, ==, 0);
    g_free (seen.last_seen);
}

int
main (int argc, char *argv[])
{
    /* gui_misc_replace_db_path persists the list on the way out. The schema is
     * installed on any machine that runs the app, so without this the suite
     * would rewrite the developer's own database list. */
    g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);

    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/db-relocation/keeps-the-row", test_replace_keeps_the_row);
    g_test_add_func ("/db-relocation/unknown-old-path", test_unknown_old_path_is_refused);
    g_test_add_func ("/db-relocation/duplicate-below", test_duplicate_below_is_absorbed);
    g_test_add_func ("/db-relocation/duplicate-above", test_duplicate_above_is_absorbed);
    g_test_add_func ("/db-relocation/duplicates-both-sides", test_duplicates_on_both_sides);
    g_test_add_func ("/db-relocation/onto-itself", test_relocating_onto_itself_keeps_the_row);

    g_test_add_func ("/db-relocation/primary/handed-over", test_absorbed_duplicate_hands_over_primary);
    g_test_add_func ("/db-relocation/primary/left-alone", test_absorbing_a_plain_duplicate_leaves_primary_alone);

    g_test_add_func ("/db-relocation/notify/path-announced", test_path_change_is_announced);
    g_test_add_func ("/db-relocation/notify/unchanged-is-quiet", test_unchanged_path_is_not_announced);

    return g_test_run ();
}
