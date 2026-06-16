#include <glib.h>
#include <glib/gstdio.h>
#include "common.h"
#include "get-providers-data.h"
#include "gquarks.h"

static gchar *
write_tmp_json (const gchar *name,
                const gchar *contents,
                gchar      **dir_out)
{
    GError *err = NULL;
    gchar *dir = g_dir_make_tmp ("otpclient-aegis-test-XXXXXX", &err);
    g_assert_no_error (err);
    g_assert_nonnull (dir);

    gchar *path = g_build_filename (dir, name, NULL);
    g_assert_true (g_file_set_contents (path, contents, -1, &err));
    g_assert_no_error (err);

    *dir_out = dir;
    return path;
}

static void
cleanup_tmp_json (gchar *dir,
                  gchar *path)
{
    g_unlink (path);
    g_rmdir (dir);
    g_free (path);
    g_free (dir);
}

static void
assert_encrypted_aegis_rejected (const gchar *contents)
{
    gchar *dir = NULL;
    gchar *path = write_tmp_json ("aegis-encrypted.json", contents, &dir);

    GError *err = NULL;
    GSList *otps = get_aegis_data (path, "password", DEFAULT_MEMLOCK_VALUE, 0, &err);
    g_assert_null (otps);
    g_assert_error (err, generic_error_gquark (), GENERIC_ERRCODE);
    g_clear_error (&err);

    cleanup_tmp_json (dir, path);
}

static void
test_aegis_missing_entries_rejected (void)
{
    gchar *dir = NULL;
    gchar *path = write_tmp_json ("aegis.json", "{\"db\":{}}", &dir);

    GError *err = NULL;
    GSList *otps = get_aegis_data (path, NULL, DEFAULT_MEMLOCK_VALUE, 0, &err);
    g_assert_null (otps);
    g_assert_error (err, generic_error_gquark (), GENERIC_ERRCODE);
    g_clear_error (&err);

    cleanup_tmp_json (dir, path);
}

static void
test_aegis_bad_scrypt_n_rejected (void)
{
    assert_encrypted_aegis_rejected (
        "{\"header\":{\"slots\":[{\"type\":1,\"n\":1000,\"p\":1}],\"params\":{}},\"db\":\"\"}");
}

static void
test_aegis_bad_scrypt_p_rejected (void)
{
    assert_encrypted_aegis_rejected (
        "{\"header\":{\"slots\":[{\"type\":1,\"n\":1024,\"p\":17}],\"params\":{}},\"db\":\"\"}");
}

static void
test_aegis_short_hex_rejected (void)
{
    assert_encrypted_aegis_rejected (
        "{\"header\":{\"slots\":[{\"type\":1,\"n\":1024,\"p\":1,"
        "\"salt\":\"00\","
        "\"key\":\"0000000000000000000000000000000000000000000000000000000000000000\","
        "\"key_params\":{\"nonce\":\"000000000000000000000000\",\"tag\":\"00000000000000000000000000000000\"}}],"
        "\"params\":{\"nonce\":\"000000000000000000000000\",\"tag\":\"00000000000000000000000000000000\"}},"
        "\"db\":\"\"}");
}

static void
test_aegis_missing_token_fields_skipped (void)
{
    gchar *dir = NULL;
    gchar *path = write_tmp_json (
        "aegis-bad-token.json",
        "{\"db\":{\"entries\":[{\"type\":\"TOTP\",\"issuer\":\"Example\",\"name\":\"alice\","
        "\"info\":{\"digits\":6,\"period\":30}}]}}",
        &dir);

    GError *err = NULL;
    GSList *otps = get_aegis_data (path, NULL, DEFAULT_MEMLOCK_VALUE, 0, &err);
    g_assert_null (otps);
    g_assert_no_error (err);

    cleanup_tmp_json (dir, path);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    gchar *init_err = init_libs (DEFAULT_MEMLOCK_VALUE);
    g_assert_null (init_err);

    g_test_add_func ("/malformed-aegis/missing-entries", test_aegis_missing_entries_rejected);
    g_test_add_func ("/malformed-aegis/bad-scrypt-n", test_aegis_bad_scrypt_n_rejected);
    g_test_add_func ("/malformed-aegis/bad-scrypt-p", test_aegis_bad_scrypt_p_rejected);
    g_test_add_func ("/malformed-aegis/short-hex", test_aegis_short_hex_rejected);
    g_test_add_func ("/malformed-aegis/missing-token-fields", test_aegis_missing_token_fields_skipped);

    return g_test_run ();
}
