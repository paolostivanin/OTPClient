#include <glib.h>
#include <glib/gstdio.h>
#include "common.h"
#include "get-providers-data.h"
#include "gquarks.h"

static gchar *
write_tmp_json (const gchar  *name,
                const gchar  *contents,
                gchar       **dir_out)
{
    GError *err = NULL;
    gchar *dir = g_dir_make_tmp ("otpclient-importers-test-XXXXXX", &err);
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

/* ---------- Authenticator Pro ---------- */

static void
test_authpro_missing_authenticators (void)
{
    /* Plain (non-encrypted) Authenticator Pro JSON missing the required
     * top-level "Authenticators" array. parse_authpro_json_data must reject
     * the file with generic_error_gquark and not crash on the NULL array. */
    gchar *dir = NULL;
    gchar *path = write_tmp_json ("authpro-no-auths.json", "{\"Categories\":[]}", &dir);

    GError *err = NULL;
    GSList *otps = get_authpro_data (path, NULL, DEFAULT_MEMLOCK_VALUE, 0, &err);
    g_assert_null (otps);
    g_assert_error (err, generic_error_gquark (), GENERIC_ERRCODE);
    g_clear_error (&err);

    cleanup_tmp_json (dir, path);
}

static void
test_authpro_bad_algorithm_enum (void)
{
    /* Algorithm 99 is outside the 0/1/2 mapping (SHA1/SHA256/SHA512) and
     * Type 2 is TOTP. The entry must be skipped, leaving an empty result
     * without any error. */
    gchar *dir = NULL;
    gchar *path = write_tmp_json ("authpro-bad-algo.json",
        "{\"Authenticators\":[{"
            "\"Secret\":\"JBSWY3DPEHPK3PXP\","
            "\"Issuer\":\"Example\",\"Username\":\"alice\","
            "\"Digits\":6,\"Period\":30,\"Counter\":0,"
            "\"Algorithm\":99,\"Type\":2}]}",
        &dir);

    GError *err = NULL;
    GSList *otps = get_authpro_data (path, NULL, DEFAULT_MEMLOCK_VALUE, 0, &err);
    g_assert_null (otps);
    g_assert_no_error (err);

    cleanup_tmp_json (dir, path);
}

static void
test_authpro_missing_secret_field (void)
{
    /* An entry without a "Secret" string must be skipped silently;
     * the surrounding parse must continue and return what it has. */
    gchar *dir = NULL;
    gchar *path = write_tmp_json ("authpro-no-secret.json",
        "{\"Authenticators\":[{"
            "\"Issuer\":\"Example\",\"Username\":\"alice\","
            "\"Digits\":6,\"Period\":30,\"Counter\":0,"
            "\"Algorithm\":0,\"Type\":2}]}",
        &dir);

    GError *err = NULL;
    GSList *otps = get_authpro_data (path, NULL, DEFAULT_MEMLOCK_VALUE, 0, &err);
    g_assert_null (otps);
    g_assert_no_error (err);

    cleanup_tmp_json (dir, path);
}

static void
test_authpro_unsupported_type_enum (void)
{
    /* Type 3 (Mobile-OTP) and 5 (Yandex) are not supported. The entry must
     * be skipped silently. */
    gchar *dir = NULL;
    gchar *path = write_tmp_json ("authpro-bad-type.json",
        "{\"Authenticators\":[{"
            "\"Secret\":\"JBSWY3DPEHPK3PXP\","
            "\"Issuer\":\"Example\",\"Username\":\"alice\","
            "\"Digits\":6,\"Period\":30,\"Counter\":0,"
            "\"Algorithm\":0,\"Type\":3}]}",
        &dir);

    GError *err = NULL;
    GSList *otps = get_authpro_data (path, NULL, DEFAULT_MEMLOCK_VALUE, 0, &err);
    g_assert_null (otps);
    g_assert_no_error (err);

    cleanup_tmp_json (dir, path);
}

/* ---------- 2FAS ---------- */

static void
test_twofas_invalid_schema_version (void)
{
    /* schemaVersion != 4 must be reported as an import error rather than
     * masquerading as a successful import of zero tokens. */
    gchar *dir = NULL;
    gchar *path = write_tmp_json ("twofas-bad-schema.json",
        "{\"schemaVersion\":99,\"services\":[]}", &dir);

    GError *err = NULL;
    GSList *otps = get_twofas_data (path, NULL, 0, &err);
    g_assert_null (otps);
    g_assert_error (err, generic_error_gquark (), GENERIC_ERRCODE);
    g_clear_error (&err);

    cleanup_tmp_json (dir, path);
}

static void
test_twofas_malformed_servicesencrypted (void)
{
    /* Encrypted backup with servicesEncrypted lacking the required three
     * colon-separated base64 fields. Must surface a generic error. */
    gchar *dir = NULL;
    gchar *path = write_tmp_json ("twofas-bad-enc.json",
        "{\"schemaVersion\":4,\"servicesEncrypted\":\"only-one-field\"}", &dir);

    GError *err = NULL;
    GSList *otps = get_twofas_data (path, "password", 0, &err);
    g_assert_null (otps);
    g_assert_error (err, generic_error_gquark (), GENERIC_ERRCODE);
    g_clear_error (&err);

    cleanup_tmp_json (dir, path);
}

static void
test_twofas_bad_authentication_tag (void)
{
    guchar ciphertext[32] = {0};
    guchar salt[256] = {0};
    guchar iv[12] = {0};
    g_autofree gchar *c64 = g_base64_encode (ciphertext, sizeof ciphertext);
    g_autofree gchar *s64 = g_base64_encode (salt, sizeof salt);
    g_autofree gchar *i64 = g_base64_encode (iv, sizeof iv);
    g_autofree gchar *json = g_strdup_printf (
        "{\"schemaVersion\":4,\"servicesEncrypted\":\"%s:%s:%s\"}",
        c64, s64, i64);
    gchar *dir = NULL;
    gchar *path = write_tmp_json ("twofas-bad-tag.json", json, &dir);

    GError *err = NULL;
    GSList *otps = get_twofas_data (path, "wrong-password", 0, &err);
    g_assert_null (otps);
    g_assert_nonnull (err);
    g_clear_error (&err);
    cleanup_tmp_json (dir, path);
}

static void
test_twofas_unsupported_tokentype (void)
{
    /* A service with tokenType "BOGUS" must be skipped without crashing.
     * The required algorithm and otp wrapper are present so only the
     * tokenType branch fails. */
    gchar *dir = NULL;
    gchar *path = write_tmp_json ("twofas-bad-token.json",
        "{\"schemaVersion\":4,\"services\":[{"
            "\"secret\":\"JBSWY3DPEHPK3PXP\","
            "\"otp\":{\"tokenType\":\"BOGUS\","
                     "\"algorithm\":\"SHA1\","
                     "\"digits\":6,\"period\":30,"
                     "\"issuer\":\"Example\",\"account\":\"alice\"}}]}",
        &dir);

    GError *err = NULL;
    GSList *otps = get_twofas_data (path, NULL, 0, &err);
    g_assert_null (otps);
    g_assert_no_error (err);

    cleanup_tmp_json (dir, path);
}

static void
test_twofas_missing_services (void)
{
    /* Plain 2FAS backup without a "services" array. Must surface a clean
     * generic error instead of crashing on the NULL lookup. */
    gchar *dir = NULL;
    gchar *path = write_tmp_json ("twofas-no-services.json",
        "{\"schemaVersion\":4}", &dir);

    GError *err = NULL;
    GSList *otps = get_twofas_data (path, NULL, 0, &err);
    g_assert_null (otps);
    g_assert_error (err, generic_error_gquark (), GENERIC_ERRCODE);
    g_clear_error (&err);

    cleanup_tmp_json (dir, path);
}

/* ---------- FreeOTP+ ---------- */

static void
test_freeotp_mixed_valid_invalid (void)
{
    /* FreeOTP+ exports are otpauth:// URIs, one per line. Mix one valid
     * TOTP URI, one URI with an invalid base32 secret, and one non-URI
     * line. The parser must keep the one valid entry and silently drop
     * the rest without crashing. This complements the unit-level
     * test_parse_uri tests by exercising the file-reading path
     * (path_open_safe_regular_file + get_otpauth_data).
     *
     * Passes db_size=0 deliberately: get_freeotpplus_data must include the
     * file size in its secmem budget so a fresh import into an empty
     * database doesn't trip the "not enough secmem" branch with a NULL
     * gcry_malloc_secure(0). */
    gchar *dir = NULL;
    gchar *path = write_tmp_json ("freeotp-mixed.txt",
        "otpauth://totp/Example:alice?secret=JBSWY3DPEHPK3PXP&issuer=Example\n"
        "otpauth://totp/Example:bad?secret=00000000&issuer=Example\n"
        "not-a-uri-at-all\n",
        &dir);

    GError *err = NULL;
    GSList *otps = get_freeotpplus_data (path, DEFAULT_MEMLOCK_VALUE, 0, &err);
    g_assert_no_error (err);
    g_assert_nonnull (otps);
    g_assert_cmpuint (g_slist_length (otps), ==, 1);

    otp_t *otp = otps->data;
    g_assert_cmpstr (otp->account_name, ==, "alice");
    g_assert_cmpstr (otp->secret, ==, "JBSWY3DPEHPK3PXP");

    free_otps_gslist (otps, 1);
    cleanup_tmp_json (dir, path);
}

static void
test_freeotp_all_invalid (void)
{
    /* When no line yields a valid token, get_freeotpplus_data returns
     * NULL with no error set: the parser simply found nothing. */
    gchar *dir = NULL;
    gchar *path = write_tmp_json ("freeotp-all-bad.txt",
        "not-a-uri\n"
        "still-not-a-uri\n",
        &dir);

    GError *err = NULL;
    GSList *otps = get_freeotpplus_data (path, DEFAULT_MEMLOCK_VALUE, 0, &err);
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

    g_test_add_func ("/malformed-importers/authpro/missing-authenticators",
                     test_authpro_missing_authenticators);
    g_test_add_func ("/malformed-importers/authpro/bad-algorithm-enum",
                     test_authpro_bad_algorithm_enum);
    g_test_add_func ("/malformed-importers/authpro/missing-secret",
                     test_authpro_missing_secret_field);
    g_test_add_func ("/malformed-importers/authpro/unsupported-type",
                     test_authpro_unsupported_type_enum);

    g_test_add_func ("/malformed-importers/twofas/invalid-schema",
                     test_twofas_invalid_schema_version);
    g_test_add_func ("/malformed-importers/twofas/malformed-services-encrypted",
                     test_twofas_malformed_servicesencrypted);
    g_test_add_func ("/malformed-importers/twofas/bad-tag",
                     test_twofas_bad_authentication_tag);
    g_test_add_func ("/malformed-importers/twofas/unsupported-tokentype",
                     test_twofas_unsupported_tokentype);
    g_test_add_func ("/malformed-importers/twofas/missing-services",
                     test_twofas_missing_services);

    g_test_add_func ("/malformed-importers/freeotp/mixed-valid-invalid",
                     test_freeotp_mixed_valid_invalid);
    g_test_add_func ("/malformed-importers/freeotp/all-invalid",
                     test_freeotp_all_invalid);

    return g_test_run ();
}
