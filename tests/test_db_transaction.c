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

static void
test_lock_unsupported_fallback (void)
{
    gchar *dir = NULL;
    gchar *path = NULL;
    DatabaseData *db_data = make_db_data (&dir, &path);

    /* Simulate a filesystem whose flock() fails with ENOSYS, e.g. the Flatpak
     * document-portal FUSE mount (issue #466). The transaction must still
     * succeed and write the database instead of aborting. */
    db_test_set_unsupported_lock (TRUE);

    /* The fallback warns exactly once per process; expect it on the first write. */
    g_test_expect_message (NULL, G_LOG_LEVEL_WARNING, "*lock not supported*");

    GError *err = NULL;
    g_assert_true (db_transaction (db_data, append_token_mutation, NULL, &err));
    g_assert_no_error (err);
    g_test_assert_expected_messages ();
    g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));
    g_assert_cmpint ((int) json_array_size (db_data->in_memory_json_data), ==, 2);

    /* A second write must also succeed and must NOT warn again (warn-once);
     * an unexpected warning here would be fatal under the test harness. */
    g_assert_true (db_transaction (db_data, append_token_mutation, NULL, &err));
    g_assert_no_error (err);
    g_assert_cmpint ((int) json_array_size (db_data->in_memory_json_data), ==, 3);

    db_test_set_unsupported_lock (FALSE);
    cleanup_db_data (db_data, dir, path);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    gchar *init_err = init_libs (DEFAULT_MEMLOCK_VALUE);
    g_assert_null (init_err);

    g_test_add_func ("/db-transaction/encrypt-failure", test_encrypt_failure_preserves_add_edit_delete);
    g_test_add_func ("/db-transaction/atomic-write-failure", test_atomic_write_failure_preserves_add);
    g_test_add_func ("/db-transaction/password-change-failure", test_password_change_failure_restores_key);
    g_test_add_func ("/db-transaction/kdf-failure", test_kdf_failure_restores_params);
    g_test_add_func ("/db-transaction/stale-snapshot", test_stale_snapshot_rejected);
    g_test_add_func ("/db-transaction/lock-unsupported-fallback", test_lock_unsupported_fallback);

    return g_test_run ();
}
