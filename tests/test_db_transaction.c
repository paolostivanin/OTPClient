#include <glib.h>
#include <glib/gstdio.h>
#include <jansson.h>
#include "common.h"
#include "db-common.h"
#include "gquarks.h"

static json_t *
valid_totp (const gchar *label)
{
    return build_json_obj ("TOTP", label, "Example", "JBSWY3DPEHPK3PXP",
                           6, "SHA1", 30, 0, NULL);
}

static DatabaseData *
make_db_data (gchar **dir_out,
              gchar **path_out)
{
    GError *err = NULL;
    gchar *dir = g_dir_make_tmp ("otpclient-db-test-XXXXXX", &err);
    g_assert_no_error (err);
    g_assert_nonnull (dir);

    gchar *path = g_build_filename (dir, "test.enc", NULL);
    DatabaseData *db_data = database_data_new (path, DEFAULT_MEMLOCK_VALUE);
    db_data->key = secure_strdup ("old-password");
    db_data->argon2id_iter = ARGON2ID_MIN_ITER;
    db_data->argon2id_memcost = ARGON2ID_MIN_MC;
    db_data->argon2id_parallelism = ARGON2ID_MIN_PARAL;
    db_data->current_db_version = DB_VERSION;
    db_data->in_memory_json_data = json_array ();
    json_array_append_new (db_data->in_memory_json_data, valid_totp ("alice"));

    *dir_out = dir;
    *path_out = path;
    return db_data;
}

static void
cleanup_db_data (DatabaseData *db_data,
                 gchar        *dir,
                 gchar        *path)
{
    database_data_free (db_data);
    g_unlink (path);
    g_autofree gchar *lock_path = g_strconcat (path, ".lock", NULL);
    g_unlink (lock_path);
    g_rmdir (dir);
    g_free (path);
    g_free (dir);
}

static gboolean
append_token_mutation (json_t   *candidate,
                       gpointer  user_data,
                       GError  **err)
{
    (void) user_data;
    (void) err;
    json_array_append_new (candidate, valid_totp ("bob"));
    return TRUE;
}

static gboolean
edit_token_mutation (json_t   *candidate,
                     gpointer  user_data,
                     GError  **err)
{
    (void) user_data;
    (void) err;
    json_t *obj = json_array_get (candidate, 0);
    json_object_set_new (obj, "label", json_string ("alice-edited"));
    return TRUE;
}

static gboolean
delete_token_mutation (json_t   *candidate,
                       gpointer  user_data,
                       GError  **err)
{
    (void) user_data;
    (void) err;
    json_array_remove (candidate, 0);
    return TRUE;
}

static void
assert_transaction_failure_preserves_json (DbMutationFunc mutation,
                                           gboolean       fail_atomic_write)
{
    gchar *dir = NULL;
    gchar *path = NULL;
    DatabaseData *db_data = make_db_data (&dir, &path);
    json_t *before = json_deep_copy (db_data->in_memory_json_data);

    GError *err = NULL;
    if (fail_atomic_write)
        db_test_set_fail_atomic_write (TRUE);
    else
        db_test_set_fail_encrypt (TRUE);

    g_assert_false (db_transaction (db_data, mutation, NULL, &err));
    g_assert_error (err, generic_error_gquark (), GENERIC_ERRCODE);
    g_clear_error (&err);
    db_test_set_fail_encrypt (FALSE);
    db_test_set_fail_atomic_write (FALSE);

    g_assert_true (json_equal (db_data->in_memory_json_data, before));
    g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

    json_decref (before);
    cleanup_db_data (db_data, dir, path);
}

static void
test_encrypt_failure_preserves_add_edit_delete (void)
{
    assert_transaction_failure_preserves_json (append_token_mutation, FALSE);
    assert_transaction_failure_preserves_json (edit_token_mutation, FALSE);
    assert_transaction_failure_preserves_json (delete_token_mutation, FALSE);
}

static void
test_atomic_write_failure_preserves_add (void)
{
    assert_transaction_failure_preserves_json (append_token_mutation, TRUE);
}

/* update_db(), not only db_transaction(), owns rollback of direct live-JSON
 * mutations. GUI callers must therefore not manually reinsert an item after a
 * failed delete: the committed snapshot already restored it, and reinserting
 * would create a duplicate. */
static void
test_update_failure_restores_direct_delete_once (void)
{
    gchar *dir = NULL;
    gchar *path = NULL;
    DatabaseData *db_data = make_db_data (&dir, &path);
    GError *err = NULL;

    /* Establish both the on-disk database and its committed in-memory
     * snapshot, then mutate the live array in the same way as move-token. */
    update_db (db_data, &err);
    g_assert_no_error (err);
    json_t *before = json_deep_copy (db_data->in_memory_json_data);
    g_assert_cmpuint (json_array_size (before), ==, 1);
    json_array_remove (db_data->in_memory_json_data, 0);

    db_test_set_fail_atomic_write (TRUE);
    update_db (db_data, &err);
    db_test_set_fail_atomic_write (FALSE);
    g_assert_error (err, generic_error_gquark (), GENERIC_ERRCODE);
    g_clear_error (&err);

    g_assert_true (json_equal (db_data->in_memory_json_data, before));
    g_assert_cmpuint (json_array_size (db_data->in_memory_json_data), ==, 1);

    json_decref (before);
    cleanup_db_data (db_data, dir, path);
}

static void
test_password_change_failure_restores_key (void)
{
    gchar *dir = NULL;
    gchar *path = NULL;
    DatabaseData *db_data = make_db_data (&dir, &path);
    json_t *before = json_deep_copy (db_data->in_memory_json_data);

    db_test_set_fail_encrypt (TRUE);
    GError *err = NULL;
    g_assert_false (db_change_password (db_data, "new-password", &err));
    g_assert_error (err, generic_error_gquark (), GENERIC_ERRCODE);
    g_clear_error (&err);
    db_test_set_fail_encrypt (FALSE);

    g_assert_cmpstr (db_data->key, ==, "old-password");
    g_assert_true (json_equal (db_data->in_memory_json_data, before));

    json_decref (before);
    cleanup_db_data (db_data, dir, path);
}

static void
test_kdf_failure_restores_params (void)
{
    gchar *dir = NULL;
    gchar *path = NULL;
    DatabaseData *db_data = make_db_data (&dir, &path);
    json_t *before = json_deep_copy (db_data->in_memory_json_data);

    db_test_set_fail_encrypt (TRUE);
    GError *err = NULL;
    g_assert_false (db_update_kdf_params (db_data, 2, ARGON2ID_MIN_MC * 2, 1, &err));
    g_assert_error (err, generic_error_gquark (), GENERIC_ERRCODE);
    g_clear_error (&err);
    db_test_set_fail_encrypt (FALSE);

    g_assert_cmpint (db_data->argon2id_iter, ==, ARGON2ID_MIN_ITER);
    g_assert_cmpint (db_data->argon2id_memcost, ==, ARGON2ID_MIN_MC);
    g_assert_cmpint (db_data->argon2id_parallelism, ==, ARGON2ID_MIN_PARAL);
    g_assert_true (json_equal (db_data->in_memory_json_data, before));

    json_decref (before);
    cleanup_db_data (db_data, dir, path);
}

static void
test_stale_snapshot_rejected (void)
{
    gchar *dir = NULL;
    gchar *path = NULL;
    DatabaseData *first = make_db_data (&dir, &path);

    GError *err = NULL;
    update_db (first, &err);
    g_assert_no_error (err);
    g_assert_true (first->has_loaded_file_digest);

    DatabaseData *second = database_data_new (path, DEFAULT_MEMLOCK_VALUE);
    second->key = secure_strdup ("old-password");
    load_db (second, &err);
    g_assert_no_error (err);
    g_assert_true (second->has_loaded_file_digest);

    g_assert_true (db_transaction (second, append_token_mutation, NULL, &err));
    g_assert_no_error (err);

    json_t *before = json_deep_copy (first->in_memory_json_data);
    g_assert_false (db_transaction (first, append_token_mutation, NULL, &err));
    g_assert_error (err, generic_error_gquark (), GENERIC_ERRCODE);
    g_assert_true (json_equal (first->in_memory_json_data, before));
    g_clear_error (&err);

    json_decref (before);
    database_data_free (second);
    cleanup_db_data (first, dir, path);
}

/* Number of *.lock files under XDG_DATA_HOME/otpclient/locks. */
static guint
count_fallback_locks (void)
{
    g_autofree gchar *dir = g_build_filename (g_get_user_data_dir (), "otpclient", "locks", NULL);
    GDir *d = g_dir_open (dir, 0, NULL);
    if (d == NULL)
        return 0;

    guint n = 0;
    const gchar *name;
    while ((name = g_dir_read_name (d)) != NULL) {
        if (g_str_has_suffix (name, ".lock"))
            n++;
    }
    g_dir_close (d);
    return n;
}

/* A database on a filesystem that cannot lock, which is what the Flatpak
 * document portal is: flock() there fails with ENOSYS (issue #466). The write
 * must succeed, and the lock must be taken in the user data dir instead of
 * being skipped, so a second OTPClient process is still kept out. */
static void
test_lock_unsupported_uses_fallback (void)
{
    gchar *dir = NULL;
    gchar *path = NULL;
    DatabaseData *db_data = make_db_data (&dir, &path);

    guint locks_before = count_fallback_locks ();
    db_test_set_lock_mode (DB_TEST_LOCK_UNSUPPORTED_BESIDE_DB);

    /* No warning at all on this path: an unexpected one is fatal under the
     * test harness, which is the assertion that the fallback really worked. */
    GError *err = NULL;
    g_assert_true (db_transaction (db_data, append_token_mutation, NULL, &err));
    g_assert_no_error (err);
    g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));
    g_assert_cmpint ((int) json_array_size (db_data->in_memory_json_data), ==, 2);
    g_assert_cmpuint (count_fallback_locks (), ==, locks_before + 1);

    /* The lock file is keyed on the database path, so a second write reuses it
     * rather than accumulating one file per save. */
    g_assert_true (db_transaction (db_data, append_token_mutation, NULL, &err));
    g_assert_no_error (err);
    g_assert_cmpint ((int) json_array_size (db_data->in_memory_json_data), ==, 3);
    g_assert_cmpuint (count_fallback_locks (), ==, locks_before + 1);

    db_test_set_lock_mode (DB_TEST_LOCK_SUPPORTED);
    cleanup_db_data (db_data, dir, path);
}

/* Nowhere can lock, e.g. an NFS home with no lock daemon. The lock is a guard
 * against a concurrent writer, not a correctness requirement, so the write must
 * still go through, with one warning and not one per save. */
static void
test_lock_unsupported_everywhere_warns_once (void)
{
    gchar *dir = NULL;
    gchar *path = NULL;
    DatabaseData *db_data = make_db_data (&dir, &path);

    db_test_set_lock_mode (DB_TEST_LOCK_UNSUPPORTED_EVERYWHERE);
    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*no fallback lock could be taken*");

    GError *err = NULL;
    g_assert_true (db_transaction (db_data, append_token_mutation, NULL, &err));
    g_assert_no_error (err);
    g_test_assert_expected_messages ();
    g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));
    g_assert_cmpint ((int) json_array_size (db_data->in_memory_json_data), ==, 2);

    /* Warn-once: a second warning here would be an unexpected message and fatal. */
    g_assert_true (db_transaction (db_data, append_token_mutation, NULL, &err));
    g_assert_no_error (err);
    g_assert_cmpint ((int) json_array_size (db_data->in_memory_json_data), ==, 3);

    db_test_set_lock_mode (DB_TEST_LOCK_SUPPORTED);
    cleanup_db_data (db_data, dir, path);
}

/* The v1/v2 -> v3 migration decrefs in_memory_json_data before re-reading the
 * file it just rewrote. If that re-read fails, load_db used to return with the
 * pointer still set, and database_data_purge_secrets decrefed the same object a
 * second time. Under ASan this aborts; without it, it silently corrupts the
 * jansson allocator, which here is libgcrypt secure memory. */
static void
test_migration_decrypt_failure_no_double_free (void)
{
    gchar *dir = NULL;
    gchar *path = NULL;
    DatabaseData *db_data = make_db_data (&dir, &path);

    /* Write a real database first, so load_db has something to read. */
    GError *err = NULL;
    g_assert_true (db_transaction (db_data, append_token_mutation, NULL, &err));
    g_assert_no_error (err);
    database_data_free (db_data);

    db_data = database_data_new (path, DEFAULT_MEMLOCK_VALUE);
    db_data->key = secure_strdup ("old-password");
    db_data->argon2id_iter = ARGON2ID_MIN_ITER;
    db_data->argon2id_memcost = ARGON2ID_MIN_MC;
    db_data->argon2id_parallelism = ARGON2ID_MIN_PARAL;

    /* load_db decrypts once to read the file, then again after the migration
     * rewrite. Let the first through and fail the second, which is the one that
     * runs with in_memory_json_data already decrefed. */
    db_test_set_force_migration (TRUE);
    db_test_fail_decrypt_after (1);

    load_db (db_data, &err);
    g_assert_nonnull (err);
    g_clear_error (&err);

    db_test_fail_decrypt_after (-1);
    db_test_set_force_migration (FALSE);

    /* The contract the fix restores: nothing left pointing at freed memory, so
     * the free below is a single decref and not a second one. */
    g_assert_null (db_data->in_memory_json_data);

    cleanup_db_data (db_data, dir, path);
}

/* The lock file next to the database cannot even be created: a read-only mount,
 * a full filesystem, an over-quota home. That is a different path from ENOSYS
 * out of flock, and it used to fail lock_db without ever reaching the fallback,
 * so the save was refused outright. It must behave like ENOSYS instead: fall
 * through to the lock in the user data dir and commit. */
static void
test_lock_open_failure_uses_fallback (void)
{
    gchar *dir = NULL;
    gchar *path = NULL;
    DatabaseData *db_data = make_db_data (&dir, &path);

    guint locks_before = count_fallback_locks ();
    db_test_set_lock_mode (DB_TEST_LOCK_OPEN_FAILS_BESIDE_DB);

    /* As above, silence is the assertion: any warning here would be fatal. */
    GError *err = NULL;
    g_assert_true (db_transaction (db_data, append_token_mutation, NULL, &err));
    g_assert_no_error (err);
    g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));
    g_assert_cmpint ((int) json_array_size (db_data->in_memory_json_data), ==, 2);
    g_assert_cmpuint (count_fallback_locks (), ==, locks_before + 1);

    db_test_set_lock_mode (DB_TEST_LOCK_SUPPORTED);
    cleanup_db_data (db_data, dir, path);
}

/* Neither location will take the lock file. Same contract as
 * test_lock_unsupported_everywhere_warns_once: the write still goes through,
 * with exactly one warning. 5.1.x saved happily with no lock at all, so
 * refusing here would lose the user's edit over a best-effort guard. */
static void
test_lock_open_failure_everywhere_warns_once (void)
{
    gchar *dir = NULL;
    gchar *path = NULL;
    DatabaseData *db_data = make_db_data (&dir, &path);

    db_test_set_lock_mode (DB_TEST_LOCK_OPEN_FAILS_EVERYWHERE);
    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*no fallback lock could be taken*");

    GError *err = NULL;
    g_assert_true (db_transaction (db_data, append_token_mutation, NULL, &err));
    g_assert_no_error (err);
    g_test_assert_expected_messages ();
    g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));
    g_assert_cmpint ((int) json_array_size (db_data->in_memory_json_data), ==, 2);

    g_assert_true (db_transaction (db_data, append_token_mutation, NULL, &err));
    g_assert_no_error (err);
    g_assert_cmpint ((int) json_array_size (db_data->in_memory_json_data), ==, 3);

    db_test_set_lock_mode (DB_TEST_LOCK_SUPPORTED);
    cleanup_db_data (db_data, dir, path);
}

/* L7b: a failed append while merging quarantined tokens for serialization must
 * abort the save (nothing is on disk yet) instead of silently omitting the
 * token from the encrypted file, where the next reload would lose it forever. */
static void
test_quarantine_append_failure_aborts_save (void)
{
    gchar *dir = NULL;
    gchar *path = NULL;
    DatabaseData *db_data = make_db_data (&dir, &path);

    /* A token that failed validation on load and was set aside (issue #464):
     * encrypt_db merges it back in for serialization only. */
    db_data->quarantined_tokens = json_array ();
    json_array_append_new (db_data->quarantined_tokens,
                           valid_totp ("quarantined"));

    db_test_set_fail_quarantine_append (TRUE);
    GError *err = NULL;
    update_db (db_data, &err);
    db_test_set_fail_quarantine_append (FALSE);
    g_assert_error (err, generic_error_gquark (), GENERIC_ERRCODE);
    g_clear_error (&err);
    g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));

    /* Without the injected failure the save goes through. */
    update_db (db_data, &err);
    g_assert_no_error (err);
    g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));

    cleanup_db_data (db_data, dir, path);
}

/* L8: the post-save digest baseline is taken from the exact bytes the atomic
 * commit wrote. A normal save must arm the guard so an external modification
 * is still detected, and a failing baseline installation must leave the guard
 * explicitly disarmed instead of armed with a stale pre-write hash, which used
 * to block every later save with "Database changed on disk". */
static void
test_committed_digest_baseline (void)
{
    gchar *dir = NULL;
    gchar *path = NULL;
    DatabaseData *db_data = make_db_data (&dir, &path);

    GError *err = NULL;
    update_db (db_data, &err);
    g_assert_no_error (err);
    g_assert_true (db_data->has_loaded_file_digest);

    /* Tamper with the file behind the handle's back: the baseline must match
     * what the commit actually wrote, so the next save is refused. */
    GError *write_err = NULL;
    g_assert_true (g_file_set_contents (path, "tampered", -1, &write_err));
    g_assert_no_error (write_err);
    update_db (db_data, &err);
    g_assert_nonnull (err);
    g_assert_error (err, generic_error_gquark (), GENERIC_ERRCODE);
    g_clear_error (&err);
    cleanup_db_data (db_data, dir, path);

    /* Injected baseline failure: the save still commits (the write has already
     * happened at that point and cannot be rolled into a save failure), the
     * guard is explicitly disarmed, and the next save is not blocked. */
    db_data = make_db_data (&dir, &path);
    update_db (db_data, &err);
    g_assert_no_error (err);

    db_test_set_fail_committed_digest (TRUE);
    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING,
                           "*Could not baseline the committed database file*");
    update_db (db_data, &err);
    db_test_set_fail_committed_digest (FALSE);
    g_assert_no_error (err);
    g_test_assert_expected_messages ();
    g_assert_false (db_data->has_loaded_file_digest);

    update_db (db_data, &err);
    g_assert_no_error (err);
    g_assert_true (db_data->has_loaded_file_digest);

    cleanup_db_data (db_data, dir, path);
}

/* Remove the <data home>/otpclient/locks tree, then the data home itself. */
static void
cleanup_data_home (const gchar *data_home)
{
    g_autofree gchar *locks = g_build_filename (data_home, "otpclient", "locks", NULL);
    GDir *d = g_dir_open (locks, 0, NULL);
    if (d != NULL) {
        const gchar *name;
        while ((name = g_dir_read_name (d)) != NULL) {
            g_autofree gchar *f = g_build_filename (locks, name, NULL);
            g_unlink (f);
        }
        g_dir_close (d);
    }
    g_rmdir (locks);

    g_autofree gchar *app_dir = g_build_filename (data_home, "otpclient", NULL);
    g_rmdir (app_dir);
    g_rmdir (data_home);
}

int
main (int argc, char **argv)
{
    /* Before anything reads it: lock_db falls back to a lock file under the
     * user data dir, and that should not land in whoever is running the tests. */
    GError *tmp_err = NULL;
    g_autofree gchar *data_home = g_dir_make_tmp ("otpclient-test-data-XXXXXX", &tmp_err);
    g_assert_no_error (tmp_err);
    g_setenv ("XDG_DATA_HOME", data_home, TRUE);

    g_test_init (&argc, &argv, NULL);
    gchar *init_err = init_libs (DEFAULT_MEMLOCK_VALUE);
    g_assert_null (init_err);

    g_test_add_func ("/db-transaction/encrypt-failure", test_encrypt_failure_preserves_add_edit_delete);
    g_test_add_func ("/db-transaction/atomic-write-failure", test_atomic_write_failure_preserves_add);
    g_test_add_func ("/db-transaction/direct-delete-rollback", test_update_failure_restores_direct_delete_once);
    g_test_add_func ("/db-transaction/password-change-failure", test_password_change_failure_restores_key);
    g_test_add_func ("/db-transaction/kdf-failure", test_kdf_failure_restores_params);
    g_test_add_func ("/db-transaction/stale-snapshot", test_stale_snapshot_rejected);
    g_test_add_func ("/db-transaction/lock-unsupported-fallback", test_lock_unsupported_uses_fallback);
    g_test_add_func ("/db-transaction/lock-unsupported-everywhere", test_lock_unsupported_everywhere_warns_once);
    g_test_add_func ("/db-transaction/migration-decrypt-failure", test_migration_decrypt_failure_no_double_free);
    g_test_add_func ("/db-transaction/lock-open-failure-fallback", test_lock_open_failure_uses_fallback);
    g_test_add_func ("/db-transaction/lock-open-failure-everywhere", test_lock_open_failure_everywhere_warns_once);
    g_test_add_func ("/db-transaction/quarantine-append-failure", test_quarantine_append_failure_aborts_save);
    g_test_add_func ("/db-transaction/committed-digest-baseline", test_committed_digest_baseline);

    int ret = g_test_run ();
    cleanup_data_home (data_home);
    return ret;
}
