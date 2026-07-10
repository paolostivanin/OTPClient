#include <glib.h>
#include <glib/gstdio.h>
#include <jansson.h>
#include "common.h"
#include "db-common.h"
#include "otp-validation.h"
#include "gquarks.h"

/* Regression guard for the database-load lockout class (#458/#462/#464): the
 * loader validator drifted stricter than what libcotp and the app's own UI can
 * produce, so a single legitimate token bricked the whole database. Two
 * invariants pin it shut:
 *   1. Every value in libcotp's real range passes both validators (parity),
 *      so nothing a producer can emit is later rejected on load.
 *   2. A file that still contains tokens the engine cannot use opens anyway:
 *      the valid tokens load, the broken ones are quarantined and preserved
 *      across save/reload, and the open never fails. */

static json_t *
totp_obj (const gchar *label, guint digits, const gchar *algo, guint period)
{
    return build_json_obj ("TOTP", label, "Example", "JBSWY3DPEHPK3PXP",
                           digits, algo, period, 0, NULL);
}

/* Wrap a single token in a root array and run the load-time validator. Consumes
 * the token reference. */
static gboolean
root_accepts (json_t *token)
{
    json_t *root = json_array ();
    json_array_append_new (root, token);
    GError *err = NULL;
    gboolean ok = otp_validate_database_root (root, &err);
    g_clear_error (&err);
    json_decref (root);
    return ok;
}

/* Parity: every digit count and period libcotp accepts (4-10, 1-120) must pass
 * the loader validator, across all algorithms. Under the old 6-8 digits bound
 * this tripped on 4/5/9/10 - exactly the #464 lockout. */
static void
test_engine_valid_values_accepted (void)
{
    const char *algos[] = { "SHA1", "SHA256", "SHA512" };
    for (guint d = OTP_DIGITS_MIN; d <= OTP_DIGITS_MAX; d++)
        for (guint a = 0; a < G_N_ELEMENTS (algos); a++)
            g_assert_true (root_accepts (totp_obj ("acc", d, algos[a], 30)));

    const guint periods[] = { OTP_PERIOD_MIN, 15, 30, 60, 90, OTP_PERIOD_MAX };
    for (guint p = 0; p < G_N_ELEMENTS (periods); p++)
        g_assert_true (root_accepts (totp_obj ("acc", 6, "SHA1", periods[p])));
}

/* The import-path validator (save / manual-add / import) must agree with the
 * loader on the same range, so a token accepted on one path is never rejected
 * on the other. */
static void
test_import_validator_matches_range (void)
{
    for (guint d = OTP_DIGITS_MIN; d <= OTP_DIGITS_MAX; d++) {
        otp_t otp = { .type = "TOTP", .account_name = "acc", .issuer = "Example",
                      .secret = "JBSWY3DPEHPK3PXP", .digits = d, .algo = "SHA1" };
        otp.period = 30;
        GError *err = NULL;
        g_assert_true (otp_validate_import_token (&otp, &err));
        g_clear_error (&err);
    }
    const guint periods[] = { OTP_PERIOD_MIN, 30, OTP_PERIOD_MAX };
    for (guint p = 0; p < G_N_ELEMENTS (periods); p++) {
        otp_t otp = { .type = "TOTP", .account_name = "acc", .issuer = "Example",
                      .secret = "JBSWY3DPEHPK3PXP", .digits = 6, .algo = "SHA1" };
        otp.period = periods[p];
        GError *err = NULL;
        g_assert_true (otp_validate_import_token (&otp, &err));
        g_clear_error (&err);
    }
}

/* The validator must stay strict for values libcotp cannot generate, so the
 * save/import paths still refuse to introduce them. */
static void
test_out_of_range_rejected (void)
{
    g_assert_false (root_accepts (totp_obj ("acc", OTP_DIGITS_MIN - 1, "SHA1", 30)));
    g_assert_false (root_accepts (totp_obj ("acc", OTP_DIGITS_MAX + 1, "SHA1", 30)));
    g_assert_false (root_accepts (totp_obj ("acc", 6, "SHA1", OTP_PERIOD_MAX + 1)));
    g_assert_false (root_accepts (totp_obj ("acc", 6, "SHA1", 0)));
    g_assert_false (root_accepts (totp_obj ("acc", 6, "MD5", 30)));
}

/* ---- never-brick round-trip (real encrypt/decrypt) ---- */

static gchar *
make_tmp_db_dir (gchar **path_out)
{
    GError *err = NULL;
    gchar *dir = g_dir_make_tmp ("otpclient-fb-XXXXXX", &err);
    g_assert_no_error (err);
    g_assert_nonnull (dir);
    *path_out = g_build_filename (dir, "test.enc", NULL);
    return dir;
}

static void
cleanup_tmp_db (DatabaseData *db_data, gchar *dir, gchar *path)
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

static DatabaseData *
open_writer (const gchar *path, const gchar *pw)
{
    DatabaseData *db = database_data_new (path, DEFAULT_MEMLOCK_VALUE);
    db->key = secure_strdup (pw);
    db->argon2id_iter = ARGON2ID_MIN_ITER;
    db->argon2id_memcost = ARGON2ID_MIN_MC;
    db->argon2id_parallelism = ARGON2ID_MIN_PARAL;
    db->current_db_version = DB_VERSION;
    db->in_memory_json_data = json_array ();
    json_array_append_new (db->in_memory_json_data, totp_obj ("alice", 6, "SHA1", 30));
    json_array_append_new (db->in_memory_json_data, totp_obj ("bob", 8, "SHA512", 60));
    return db;
}

static DatabaseData *
open_reader (const gchar *path, const gchar *pw, GError **err)
{
    DatabaseData *db = database_data_new (path, DEFAULT_MEMLOCK_VALUE);
    db->key = secure_strdup (pw);
    load_db (db, err);
    return db;
}

static void
test_broken_tokens_quarantined_not_bricking (void)
{
    gchar *path = NULL;
    gchar *dir = make_tmp_db_dir (&path);

    /* Seed 2 valid tokens and stage 3 the engine cannot use as "quarantined",
     * so encrypt_db writes them into the file exactly as a legacy or corrupt
     * database would carry them: an out-of-range digit count, an out-of-range
     * period, and an invalid Base32 secret. */
    DatabaseData *writer = open_writer (path, "pw");
    writer->quarantined_tokens = json_array ();
    json_array_append_new (writer->quarantined_tokens, totp_obj ("bad-digits", 12, "SHA1", 30));
    json_array_append_new (writer->quarantined_tokens, totp_obj ("bad-period", 6, "SHA1", 300));
    json_array_append_new (writer->quarantined_tokens,
        build_json_obj ("TOTP", "bad-secret", "Example", "0000", 6, "SHA1", 30, 0, NULL));

    GError *err = NULL;
    update_db (writer, &err);
    g_assert_no_error (err);

    /* Fresh open must succeed (not brick), load only the valid tokens, and set
     * the broken ones aside. */
    DatabaseData *reader = open_reader (path, "pw", &err);
    g_assert_no_error (err);
    g_assert_nonnull (reader->in_memory_json_data);
    g_assert_cmpint (json_array_size (reader->in_memory_json_data), ==, 2);
    g_assert_cmpuint (db_get_quarantined_count (reader), ==, 3);

    /* Saving and reloading must preserve the broken tokens, never drop them. */
    update_db (reader, &err);
    g_assert_no_error (err);
    DatabaseData *reader2 = open_reader (path, "pw", &err);
    g_assert_no_error (err);
    g_assert_cmpint (json_array_size (reader2->in_memory_json_data), ==, 2);
    g_assert_cmpuint (db_get_quarantined_count (reader2), ==, 3);

    database_data_free (reader2);
    database_data_free (reader);
    cleanup_tmp_db (writer, dir, path);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    gchar *init_err = init_libs (DEFAULT_MEMLOCK_VALUE);
    g_assert_null (init_err);

    g_test_add_func ("/field-bounds/engine-valid-accepted", test_engine_valid_values_accepted);
    g_test_add_func ("/field-bounds/import-matches-range",  test_import_validator_matches_range);
    g_test_add_func ("/field-bounds/out-of-range-rejected", test_out_of_range_rejected);
    g_test_add_func ("/field-bounds/broken-quarantined",    test_broken_tokens_quarantined_not_bricking);

    return g_test_run ();
}
