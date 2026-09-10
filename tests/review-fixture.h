#pragma once
#include <glib/gstdio.h>
#include "common.h"
#include "db-common.h"

#define REVIEW_SECRET "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"

typedef struct {
    gchar *dir;
    gchar *path;
    gchar *password_path;
    DatabaseData *db;
} ReviewFixture;

static void
review_fixture_init (ReviewFixture *fixture)
{
    GError *err = NULL;
    fixture->dir = g_dir_make_tmp ("otpclient-flow-XXXXXX", &err);
    g_assert_no_error (err);
    fixture->path = g_build_filename (fixture->dir, "tokens.enc", NULL);
    fixture->password_path = g_build_filename (fixture->dir, "password", NULL);
    g_assert_true (g_file_set_contents (fixture->password_path, "test-password\n", -1, &err));
    g_assert_no_error (err);
    g_assert_cmpint (g_chmod (fixture->password_path, 0600), ==, 0);
    fixture->db = database_data_new (fixture->path, DEFAULT_MEMLOCK_VALUE);
    fixture->db->key = secure_strdup ("test-password");
    fixture->db->argon2id_iter = ARGON2ID_MIN_ITER;
    fixture->db->argon2id_memcost = ARGON2ID_MIN_MC;
    fixture->db->argon2id_parallelism = ARGON2ID_MIN_PARAL;
    fixture->db->in_memory_json_data = json_array ();
    json_array_append_new (fixture->db->in_memory_json_data,
        build_json_obj ("HOTP", "alice", "Example", REVIEW_SECRET, 6, "SHA1", 30, 0, NULL));
    update_db (fixture->db, &err);
    g_assert_no_error (err);
}

static void
review_fixture_clear (ReviewFixture *fixture)
{
    database_data_free (fixture->db);
    GDir *dir = g_dir_open (fixture->dir, 0, NULL);
    const gchar *name;
    while (dir != NULL && (name = g_dir_read_name (dir)) != NULL) {
        g_autofree gchar *path = g_build_filename (fixture->dir, name, NULL);
        g_remove (path);
    }
    if (dir != NULL) g_dir_close (dir);
    g_rmdir (fixture->dir);
    g_free (fixture->dir);
    g_free (fixture->path);
    g_free (fixture->password_path);
}

#ifdef REVIEW_CLI_PATH
static gchar *
review_cli (ReviewFixture *fixture, const gchar * const *options, gboolean success, gchar **stderr_out)
{
    g_autoptr (GPtrArray) args = g_ptr_array_new ();
    g_ptr_array_add (args, (gpointer) REVIEW_CLI_PATH);
    g_ptr_array_add (args, (gpointer) "--database");
    g_ptr_array_add (args, fixture->path);
    g_ptr_array_add (args, (gpointer) "--password-file");
    g_ptr_array_add (args, fixture->password_path);
    for (guint i = 0; options[i] != NULL; i++)
        g_ptr_array_add (args, (gpointer) options[i]);
    g_ptr_array_add (args, NULL);
    gchar *out = NULL;
    gint status;
    GError *err = NULL;
    g_assert_true (g_spawn_sync (NULL, (gchar **) args->pdata, NULL, 0, NULL, NULL,
                                &out, stderr_out, &status, &err));
    g_assert_no_error (err);
    if (g_spawn_check_wait_status (status, NULL) != success)
        g_test_message ("CLI status %d, stderr: %s, stdout: %s", status, *stderr_out, out);
    g_assert_cmpint (g_spawn_check_wait_status (status, NULL), ==, success);
    return out;
}
#endif
