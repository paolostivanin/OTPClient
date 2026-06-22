#include <glib.h>
#include <glib/gstdio.h>
#include "common.h"
#include "db-common.h"
#include "get-data.h"

static void
test_hotp_not_emitted_on_persist_failure (void)
{
    if (!g_test_subprocess ()) {
        g_test_trap_subprocess (NULL, 5 * G_USEC_PER_SEC, 0);
        g_test_trap_assert_passed ();
        g_test_trap_assert_stdout_unmatched ("*Current HOTP:*");
        return;
    }

    GError *err = NULL;
    gchar *dir = g_dir_make_tmp ("otpclient-cli-hotp-XXXXXX", &err);
    g_assert_no_error (err);
    g_autofree gchar *path = g_build_filename (dir, "test.enc", NULL);

    DatabaseData *db = database_data_new (path, DEFAULT_MEMLOCK_VALUE);
    db->key = secure_strdup ("password");
    db->argon2id_iter = ARGON2ID_MIN_ITER;
    db->argon2id_memcost = ARGON2ID_MIN_MC;
    db->argon2id_parallelism = ARGON2ID_MIN_PARAL;
    db->current_db_version = DB_VERSION;
    db->in_memory_json_data = json_array ();
    json_array_append_new (
        db->in_memory_json_data,
        build_json_obj ("HOTP", "alice", "Example",
                        "JBSWY3DPEHPK3PXP", 6, "SHA1", 0, 1, NULL));
    update_db (db, &err);
    g_assert_no_error (err);

    db_test_set_fail_encrypt (TRUE);
    g_assert_false (show_token (db, "alice", "Example", TRUE, FALSE,
                                OUTPUT_FORMAT_TABLE));
    db_test_set_fail_encrypt (FALSE);
    json_t *token = json_array_get (db->in_memory_json_data, 0);
    g_assert_cmpint (json_integer_value (json_object_get (token, "counter")),
                     ==, 1);

    database_data_free (db);
    g_unlink (path);
    g_autofree gchar *bak = g_strconcat (path, ".bak", NULL);
    g_autofree gchar *lock = g_strconcat (path, ".lock", NULL);
    g_unlink (bak);
    g_unlink (lock);
    g_rmdir (dir);
    g_free (dir);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    gchar *init_err = init_libs (DEFAULT_MEMLOCK_VALUE);
    g_assert_null (init_err);
    g_test_add_func ("/cli-hotp/persist-before-output",
                     test_hotp_not_emitted_on_persist_failure);
    return g_test_run ();
}
