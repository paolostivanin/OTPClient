#include <glib.h>
#include <string.h>
#include "review-fixture.h"
#include "import-export.h"
#include "get-providers-data.h"
#include "parse-uri.h"
#include "gsettings-common.h"

static void
test_hotp_transaction (void)
{
    ReviewFixture fixture;
    review_fixture_init (&fixture);
    gsize index = 0;
    GError *err = NULL;
    g_autoptr (GPtrArray) result = db_generate_hotp (fixture.db, &index, 1, &err);
    g_assert_no_error (err);
    g_assert_cmpstr (((DbHotpResult *) result->pdata[0])->code, ==, "755224");
    g_assert_cmpuint (((DbHotpResult *) result->pdata[0])->next_counter, ==, 1);

    db_test_set_fail_atomic_write (TRUE);
    g_assert_null (db_generate_hotp (fixture.db, &index, 1, &err));
    g_assert_nonnull (err);
    g_clear_error (&err);
    db_test_set_fail_atomic_write (FALSE);
    g_assert_cmpint (json_integer_value (json_object_get (json_array_get (fixture.db->in_memory_json_data, 0), "counter")), ==, 1);

    DatabaseData *other = database_data_new (fixture.path, DEFAULT_MEMLOCK_VALUE);
    other->key = secure_strdup ("test-password");
    load_db (other, &err);
    g_assert_no_error (err);
    g_clear_pointer (&result, g_ptr_array_unref);
    result = db_generate_hotp (other, &index, 1, &err);
    g_assert_no_error (err);
    g_assert_cmpstr (((DbHotpResult *) result->pdata[0])->code, ==, "287082");
    g_assert_null (db_generate_hotp (fixture.db, &index, 1, &err));
    g_assert_nonnull (err);
    g_clear_error (&err);
    database_data_free (other);
    review_fixture_clear (&fixture);
}

static void
test_exports (void)
{
    ReviewFixture fixture;
    review_fixture_init (&fixture);
    gchar *(*exporters[]) (const gchar *, const gchar *, json_t *) = {export_aegis, export_authpro, export_twofas};
    const gchar *types[] = {AEGIS_ENC_ACTION_NAME, AUTHPRO_ENC_ACTION_NAME, TWOFAS_ENC_ACTION_NAME};
    const gchar *names[] = {"aegis", "authpro", "twofas"};
    for (guint i = 0; i < G_N_ELEMENTS (exporters); i++) {
        g_autofree gchar *path = g_strdup_printf ("%s/export-%u", fixture.dir, i);
        g_autofree gchar *error = exporters[i] (path, "", fixture.db->in_memory_json_data);
        g_assert_nonnull (error);
        g_assert_false (g_file_test (path, G_FILE_TEST_EXISTS));
        g_clear_pointer (&error, g_free);
        error = exporters[i] (path, "export-password", fixture.db->in_memory_json_data);
        g_assert_null (error);
        GError *err = NULL;
        GSList *tokens = get_data_from_provider (types[i], path, "export-password", DEFAULT_MEMLOCK_VALUE, 0, &err);
        g_assert_no_error (err);
        g_assert_cmpuint (g_slist_length (tokens), ==, 1);
        g_assert_cmpstr (((otp_t *) tokens->data)->secret, ==, REVIEW_SECRET);
        free_otps_gslist (tokens, g_slist_length (tokens));

        g_autofree gchar *legacy = g_strdup_printf ("%s/fixtures/empty-password-%s", REVIEW_TEST_SOURCE, names[i]);
        tokens = get_data_from_provider (types[i], legacy, "", DEFAULT_MEMLOCK_VALUE, 0, &err);
        g_assert_no_error (err);
        g_assert_cmpuint (g_slist_length (tokens), ==, 1);
        g_assert_cmpstr (((otp_t *) tokens->data)->secret, ==, REVIEW_SECRET);
        free_otps_gslist (tokens, g_slist_length (tokens));
    }
    review_fixture_clear (&fixture);
}

static void
test_hotp_batch (void)
{
    ReviewFixture fixture;
    review_fixture_init (&fixture);
    json_array_append_new (fixture.db->in_memory_json_data,
        build_json_obj ("HOTP", "bob", "Example", REVIEW_SECRET, 6, "SHA1", 30, 1, NULL));
    GError *err = NULL;
    update_db (fixture.db, &err);
    g_assert_no_error (err);
    const gsize indices[] = {0, 1};
    db_test_set_fail_atomic_write (TRUE);
    g_assert_null (db_generate_hotp (fixture.db, indices, 2, &err));
    g_assert_nonnull (err);
    g_clear_error (&err);
    db_test_set_fail_atomic_write (FALSE);
    for (gsize i = 0; i < 2; i++)
        g_assert_cmpint (json_integer_value (json_object_get (
            json_array_get (fixture.db->in_memory_json_data, i), "counter")), ==, i);
    g_autoptr (GPtrArray) results = db_generate_hotp (fixture.db, indices, 2, &err);
    g_assert_no_error (err);
    g_assert_cmpuint (results->len, ==, 2);
    g_assert_cmpstr (((DbHotpResult *) results->pdata[0])->code, ==, "755224");
    g_assert_cmpstr (((DbHotpResult *) results->pdata[1])->code, ==, "287082");
    load_db (fixture.db, &err);
    g_assert_no_error (err);
    for (gsize i = 0; i < 2; i++)
        g_assert_cmpint (json_integer_value (json_object_get (
            json_array_get (fixture.db->in_memory_json_data, i), "counter")), ==, i + 1);
    review_fixture_clear (&fixture);
}

static void
test_provider_diagnostics (void)
{
    ReviewFixture fixture;
    review_fixture_init (&fixture);
    gchar *(*exporters[]) (const gchar *, const gchar *, json_t *) = {export_aegis, export_authpro, export_twofas};
    const gchar *types[] = {AEGIS_PLAIN_ACTION_NAME, AUTHPRO_PLAIN_ACTION_NAME, TWOFAS_PLAIN_ACTION_NAME};
    for (guint i = 0; i < G_N_ELEMENTS (exporters); i++) {
        g_autofree gchar *path = g_strdup_printf ("%s/mixed-%u", fixture.dir, i);
        g_autofree gchar *export_error = exporters[i] (path, NULL, fixture.db->in_memory_json_data);
        g_assert_null (export_error);
        json_t *root = json_load_file (path, 0, NULL);
        g_assert_nonnull (root);
        json_t *entries = i == 0 ? json_object_get (json_object_get (root, "db"), "entries")
                         : json_object_get (root, i == 1 ? "Authenticators" : "services");
        g_assert_true (json_is_array (entries));
        json_t *invalid = json_deep_copy (json_array_get (entries, 0));
        json_t *fields = i == 0 ? json_object_get (invalid, "info") : invalid;
        json_object_set_new (fields, i == 1 ? "Secret" : "secret", json_string ("NOT-BASE32"));
        json_array_append_new (entries, invalid);
        g_assert_cmpint (json_dump_file (root, path, 0), ==, 0);
        json_decref (root);
        g_autoptr (OtpImportDiagnostics) diagnostics = otp_import_diagnostics_new ();
        GError *err = NULL;
        GSList *tokens = get_data_from_provider_full (types[i], path, NULL,
            DEFAULT_MEMLOCK_VALUE, 0, diagnostics, &err);
        g_assert_no_error (err);
        g_assert_cmpuint (g_slist_length (tokens), ==, 1);
        g_assert_cmpuint (diagnostics->skipped_invalid, ==, 1);
        g_autofree gchar *details = otp_import_diagnostics_format (diagnostics);
        g_assert_nonnull (strstr (details, "Entry 2:"));
        g_assert_null (strstr (details, "NOT-BASE32"));
        free_otps_gslist (tokens, g_slist_length (tokens));
    }
    review_fixture_clear (&fixture);
}

static void
test_import_diagnostics (void)
{
    g_autoptr (OtpImportDiagnostics) diagnostics = otp_import_diagnostics_new ();
    GSList *tokens = NULL;
    set_otps_from_uris_full ("otpauth://totp/valid?secret=JBSWY3DPEHPK3PXP\n"
                             "otpauth://totp/invalid?secret=NOT-BASE32\n", &tokens, diagnostics);
    g_assert_cmpuint (g_slist_length (tokens), ==, 1);
    g_assert_cmpuint (diagnostics->skipped_invalid, ==, 1);
    g_autofree gchar *details = otp_import_diagnostics_format (diagnostics);
    g_assert_nonnull (strstr (details, "Entry 2:"));
    g_assert_null (strstr (details, "NOT-BASE32"));
    free_otps_gslist (tokens, g_slist_length (tokens));
}

static void
test_backup_history (void)
{
    g_autoptr (GSettings) settings = gsettings_common_get_settings ();
    /* No history at all reads as no backup, rather than as a backup at the
     * epoch. The old global last-export-time is gone from the schema, so there
     * is nothing left that could be mistaken for this database's history. */
    g_assert_cmpint (gsettings_common_get_database_time (settings, OTPCLIENT_BACKUP_TIMES, "/tmp/a"), ==, 0);
    gsettings_common_set_database_time (settings, OTPCLIENT_BACKUP_TIMES, "/tmp/./a", 100);
    gsettings_common_set_database_time (settings, OTPCLIENT_BACKUP_SNOOZES, "/tmp/a", 200);
    g_assert_cmpint (gsettings_common_get_database_time (settings, OTPCLIENT_BACKUP_TIMES, "/tmp/a"), ==, 100);
    g_assert_cmpint (gsettings_common_get_database_time (settings, OTPCLIENT_BACKUP_TIMES, "/tmp/b"), ==, 0);
    g_assert_cmpint (gsettings_common_get_database_time (settings, OTPCLIENT_BACKUP_SNOOZES, "/tmp/b"), ==, 0);
    gsettings_common_relocate_backup_history ("/tmp/a", "/tmp/b");
    g_assert_cmpint (gsettings_common_get_database_time (settings, OTPCLIENT_BACKUP_TIMES, "/tmp/a"), ==, 0);
    g_assert_cmpint (gsettings_common_get_database_time (settings, OTPCLIENT_BACKUP_TIMES, "/tmp/b"), ==, 100);
    g_assert_cmpint (gsettings_common_get_database_time (settings, OTPCLIENT_BACKUP_SNOOZES, "/tmp/b"), ==, 200);
}

#ifdef REVIEW_CLI_PATH
static void
test_cli_output_and_import (void)
{
    ReviewFixture fixture;
    review_fixture_init (&fixture);
    gchar *err = NULL;
    const gchar *json_args[] = {"--show", "--account", "alice", "--output=json", NULL};
    g_autofree gchar *out = review_cli (&fixture, json_args, TRUE, &err);
    g_assert_cmpstr (err, ==, "");
    g_clear_pointer (&err, g_free);
    json_t *rows = json_loads (out, 0, NULL);
    g_assert_nonnull (rows);
    g_assert_cmpstr (json_string_value (json_object_get (json_array_get (rows, 0), "current")), ==, "755224");
    json_decref (rows);
    g_clear_pointer (&out, g_free);
    const gchar *csv_args[] = {"--show", "--account", "alice", "--output=csv", NULL};
    out = review_cli (&fixture, csv_args, TRUE, &err);
    g_assert_cmpstr (err, ==, "");
    g_clear_pointer (&err, g_free);
    g_assert_cmpstr (out, ==, "type,account,issuer,current,validity_seconds,counter,next\nHOTP,alice,Example,287082,,2,\n");
    g_clear_pointer (&out, g_free);
    g_autofree gchar *input = g_build_filename (fixture.dir, "import.txt", NULL);
    g_file_set_contents (input, "otpauth://totp/bob?secret=JBSWY3DPEHPK3PXP\notpauth://totp/bad?secret=NOT-BASE32\n", -1, NULL);
    const gchar *import_args[] = {"--import", "--type", "freeotpplus_plain", "--file", input, NULL};
    out = review_cli (&fixture, import_args, TRUE, &err);
    g_assert_nonnull (strstr (out, "Added: 1, duplicates: 0, invalid: 1"));
    g_assert_nonnull (strstr (err, "Entry 2:"));
    g_clear_pointer (&err, g_free);
    g_clear_pointer (&out, g_free);
    g_file_set_contents (input, "otpauth://totp/bad?secret=NOT-BASE32\n", -1, NULL);
    out = review_cli (&fixture, import_args, FALSE, &err);
    g_assert_nonnull (strstr (err, "Entry 1:"));
    g_free (err);
    review_fixture_clear (&fixture);
}
#endif

int
main (int argc, char **argv)
{
    g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);
    g_test_init (&argc, &argv, NULL);
    gchar *error = init_libs (DEFAULT_MEMLOCK_VALUE);
    g_assert_null (error);
    g_test_add_func ("/flows/hotp-transaction", test_hotp_transaction);
    g_test_add_func ("/flows/hotp-batch", test_hotp_batch);
    g_test_add_func ("/flows/exports", test_exports);
    g_test_add_func ("/flows/provider-diagnostics", test_provider_diagnostics);
    g_test_add_func ("/flows/import-diagnostics", test_import_diagnostics);
    g_test_add_func ("/flows/backup-history", test_backup_history);
#ifdef REVIEW_CLI_PATH
    g_test_add_func ("/flows/cli-output-and-import", test_cli_output_and_import);
#endif
    return g_test_run ();
}
