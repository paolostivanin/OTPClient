#include <glib.h>
#include <glib/gstdio.h>
#include <jansson.h>
#include <stdio.h>
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
make_db_data_with_path (const gchar *path,
                        const gchar *password)
{
    DatabaseData *db_data = database_data_new (path, DEFAULT_MEMLOCK_VALUE);
    db_data->key = secure_strdup (password);
    db_data->argon2id_iter = ARGON2ID_MIN_ITER;
    db_data->argon2id_memcost = ARGON2ID_MIN_MC;
    db_data->argon2id_parallelism = ARGON2ID_MIN_PARAL;
    db_data->current_db_version = DB_VERSION;
    db_data->in_memory_json_data = json_array ();
    json_array_append_new (db_data->in_memory_json_data, valid_totp ("alice"));
    json_array_append_new (db_data->in_memory_json_data, valid_totp ("bob"));
    return db_data;
}

static gchar *
make_tmp_db_dir (gchar **path_out)
{
    GError *err = NULL;
    gchar *dir = g_dir_make_tmp ("otpclient-db-rt-XXXXXX", &err);
    g_assert_no_error (err);
    g_assert_nonnull (dir);
    *path_out = g_build_filename (dir, "test.enc", NULL);
    return dir;
}

static void
cleanup_tmp_db (DatabaseData *db_data,
                gchar        *dir,
                gchar        *path)
{
    if (db_data != NULL)
        database_data_free (db_data);
    g_unlink (path);
    g_autofree gchar *lock_path = g_strconcat (path, ".lock", NULL);
    g_unlink (lock_path);
    g_autofree gchar *bak_path = g_strconcat (path, ".bak", NULL);
    g_unlink (bak_path);
    g_rmdir (dir);
    g_free (path);
    g_free (dir);
}

static void
test_save_then_load_preserves_json (void)
{
    gchar *path = NULL;
    gchar *dir = make_tmp_db_dir (&path);

    DatabaseData *writer = make_db_data_with_path (path, "test-password");
    json_t *snapshot = json_deep_copy (writer->in_memory_json_data);

    GError *err = NULL;
    update_db (writer, &err);
    g_assert_no_error (err);
    g_assert_true (g_file_test (path, G_FILE_TEST_EXISTS));

    DatabaseData *reader = database_data_new (path, DEFAULT_MEMLOCK_VALUE);
    reader->key = secure_strdup ("test-password");
    load_db (reader, &err);
    g_assert_no_error (err);
    g_assert_nonnull (reader->in_memory_json_data);
    g_assert_true (json_equal (reader->in_memory_json_data, snapshot));

    json_decref (snapshot);
    database_data_free (reader);
    cleanup_tmp_db (writer, dir, path);
}

static void
test_wrong_password_rejected (void)
{
    gchar *path = NULL;
    gchar *dir = make_tmp_db_dir (&path);

    DatabaseData *writer = make_db_data_with_path (path, "correct-password");
    GError *err = NULL;
    update_db (writer, &err);
    g_assert_no_error (err);

    DatabaseData *reader = database_data_new (path, DEFAULT_MEMLOCK_VALUE);
    reader->key = secure_strdup ("wrong-password");
    load_db (reader, &err);
    g_assert_nonnull (err);
    g_assert_cmpuint (err->domain, ==, bad_tag_gquark ());
    g_assert_cmpint (err->code, ==, BAD_TAG_ERRCODE);
    g_assert_null (reader->in_memory_json_data);
    g_clear_error (&err);

    database_data_free (reader);
    cleanup_tmp_db (writer, dir, path);
}

static void
test_corrupted_ciphertext_rejected (void)
{
    gchar *path = NULL;
    gchar *dir = make_tmp_db_dir (&path);

    DatabaseData *writer = make_db_data_with_path (path, "test-password");
    GError *err = NULL;
    update_db (writer, &err);
    g_assert_no_error (err);

    /* Flip a single byte just past the v3 header so AES-GCM tag verification
     * fails. The header bytes themselves are authenticated additional data;
     * flipping inside them would still trip the tag check but via a different
     * path. We target the ciphertext region to assert that body corruption is
     * caught, not just header tampering. */
    FILE *fp = g_fopen (path, "r+b");
    g_assert_nonnull (fp);
    g_assert_cmpint (fseek (fp, DB_V3_HEADER_SIZE + 4, SEEK_SET), ==, 0);
    int byte = fgetc (fp);
    g_assert_cmpint (byte, !=, EOF);
    g_assert_cmpint (fseek (fp, DB_V3_HEADER_SIZE + 4, SEEK_SET), ==, 0);
    g_assert_cmpint (fputc (byte ^ 0xff, fp), !=, EOF);
    fclose (fp);

    DatabaseData *reader = database_data_new (path, DEFAULT_MEMLOCK_VALUE);
    reader->key = secure_strdup ("test-password");
    load_db (reader, &err);
    g_assert_nonnull (err);
    g_assert_cmpuint (err->domain, ==, bad_tag_gquark ());
    g_assert_null (reader->in_memory_json_data);
    g_clear_error (&err);

    database_data_free (reader);
    cleanup_tmp_db (writer, dir, path);
}

static void
test_password_change_then_reload (void)
{
    gchar *path = NULL;
    gchar *dir = make_tmp_db_dir (&path);

    DatabaseData *writer = make_db_data_with_path (path, "old-password");
    GError *err = NULL;
    update_db (writer, &err);
    g_assert_no_error (err);

    g_assert_true (db_change_password (writer, "new-password", &err));
    g_assert_no_error (err);
    g_assert_cmpstr (writer->key, ==, "new-password");

    /* Old password must fail. */
    DatabaseData *reader_old = database_data_new (path, DEFAULT_MEMLOCK_VALUE);
    reader_old->key = secure_strdup ("old-password");
    load_db (reader_old, &err);
    g_assert_nonnull (err);
    g_assert_cmpuint (err->domain, ==, bad_tag_gquark ());
    g_clear_error (&err);
    database_data_free (reader_old);

    /* New password must succeed and yield the original JSON. */
    DatabaseData *reader_new = database_data_new (path, DEFAULT_MEMLOCK_VALUE);
    reader_new->key = secure_strdup ("new-password");
    load_db (reader_new, &err);
    g_assert_no_error (err);
    g_assert_nonnull (reader_new->in_memory_json_data);
    g_assert_cmpint (json_array_size (reader_new->in_memory_json_data), ==, 2);
    database_data_free (reader_new);

    cleanup_tmp_db (writer, dir, path);
}

static void
test_kdf_param_update_then_reload (void)
{
    gchar *path = NULL;
    gchar *dir = make_tmp_db_dir (&path);

    DatabaseData *writer = make_db_data_with_path (path, "test-password");
    GError *err = NULL;
    update_db (writer, &err);
    g_assert_no_error (err);

    const gint32 new_iter = ARGON2ID_MIN_ITER + 1;
    const gint32 new_mc   = ARGON2ID_MIN_MC * 2;
    const gint32 new_para = ARGON2ID_MIN_PARAL;
    g_assert_true (db_update_kdf_params (writer, new_iter, new_mc, new_para, &err));
    g_assert_no_error (err);

    DatabaseData *reader = database_data_new (path, DEFAULT_MEMLOCK_VALUE);
    reader->key = secure_strdup ("test-password");
    load_db (reader, &err);
    g_assert_no_error (err);
    g_assert_cmpint (reader->argon2id_iter,        ==, new_iter);
    g_assert_cmpint (reader->argon2id_memcost,     ==, new_mc);
    g_assert_cmpint (reader->argon2id_parallelism, ==, new_para);
    database_data_free (reader);

    cleanup_tmp_db (writer, dir, path);
}

static void
test_purge_then_reload (void)
{
    gchar *path = NULL;
    gchar *dir = make_tmp_db_dir (&path);
    DatabaseData *db = make_db_data_with_path (path, "test-password");
    GError *err = NULL;
    update_db (db, &err);
    g_assert_no_error (err);

    database_data_purge_secrets (db);
    g_assert_null (db->key);
    g_assert_null (db->in_memory_json_data);
    g_assert_null (db->committed_json_data);
    g_assert_null (db->cached_derived_key);
    g_assert_false (db->has_cached_key);
    g_assert_nonnull (db->db_path);

    db->key = secure_strdup ("test-password");
    load_db (db, &err);
    g_assert_no_error (err);
    g_assert_cmpuint (json_array_size (db->in_memory_json_data), ==, 2);
    cleanup_tmp_db (db, dir, path);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    gchar *init_err = init_libs (DEFAULT_MEMLOCK_VALUE);
    g_assert_null (init_err);

    g_test_add_func ("/db-roundtrip/save-then-load",       test_save_then_load_preserves_json);
    g_test_add_func ("/db-roundtrip/wrong-password",       test_wrong_password_rejected);
    g_test_add_func ("/db-roundtrip/corrupted-ciphertext", test_corrupted_ciphertext_rejected);
    g_test_add_func ("/db-roundtrip/password-change",      test_password_change_then_reload);
    g_test_add_func ("/db-roundtrip/kdf-param-update",     test_kdf_param_update_then_reload);
    g_test_add_func ("/db-roundtrip/purge-then-reload",    test_purge_then_reload);

    return g_test_run ();
}
