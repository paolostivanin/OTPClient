#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include "db-common.h"
#include "gquarks.h"

static gchar *
write_tmp_file (const gchar *name,
                const void  *data,
                gsize        len,
                gchar      **dir_out)
{
    GError *err = NULL;
    gchar *dir = g_dir_make_tmp ("otpclient-malformed-db-XXXXXX", &err);
    g_assert_no_error (err);
    g_assert_nonnull (dir);

    gchar *path = g_build_filename (dir, name, NULL);
    g_assert_true (g_file_set_contents (path, data, (gssize) len, &err));
    g_assert_no_error (err);

    *dir_out = dir;
    return path;
}

static void
cleanup_tmp_file (gchar *dir,
                  gchar *path)
{
    g_unlink (path);
    g_rmdir (dir);
    g_free (path);
    g_free (dir);
}

static void
write_be32_test (guint8 *dest,
                 guint32 value)
{
    dest[0] = (guint8) ((value >> 24) & 0xff);
    dest[1] = (guint8) ((value >> 16) & 0xff);
    dest[2] = (guint8) ((value >> 8) & 0xff);
    dest[3] = (guint8) (value & 0xff);
}

static void
fill_v3_header (guint8 *bytes,
                guint32 iter,
                guint32 memcost,
                guint32 parallelism)
{
    memcpy (bytes, DB_HEADER_NAME, DB_HEADER_NAME_LEN);
    write_be32_test (bytes + DB_HEADER_NAME_LEN, DB_VERSION);

    guint8 *salt = bytes + DB_HEADER_NAME_LEN + 4 + IV_SIZE;
    write_be32_test (salt + KDF_SALT_SIZE, iter);
    write_be32_test (salt + KDF_SALT_SIZE + 4, memcost);
    write_be32_test (salt + KDF_SALT_SIZE + 8, parallelism);
}

static void
assert_load_rejected (const gchar *path,
                      gint32       max_file_size,
                      GQuark       expected_domain)
{
    DatabaseData *db_data = database_data_new (path, max_file_size);
    db_data->key = secure_strdup ("password");

    GError *err = NULL;
    load_db (db_data, &err);
    g_assert_nonnull (err);
    if (expected_domain != 0)
        g_assert_cmpuint (err->domain, ==, expected_domain);
    g_assert_null (db_data->in_memory_json_data);

    g_clear_error (&err);
    database_data_free (db_data);
}

static void
test_truncated_header_rejected (void)
{
    const guint8 bytes[] = { 'O', 'T', 'P', 'C', 'l', 'i', 'e', 'n', 't', 0x00 };
    gchar *dir = NULL;
    gchar *path = write_tmp_file ("truncated.enc", bytes, sizeof bytes, &dir);

    assert_load_rejected (path, DEFAULT_MEMLOCK_VALUE, generic_error_gquark ());
    cleanup_tmp_file (dir, path);
}

static void
test_future_version_rejected (void)
{
    guint8 bytes[DB_HEADER_NAME_LEN + sizeof (gint32)] = {0};
    memcpy (bytes, DB_HEADER_NAME, DB_HEADER_NAME_LEN);
    gint32 future = DB_VERSION + 1;
    memcpy (bytes + DB_HEADER_NAME_LEN, &future, sizeof future);

    gchar *dir = NULL;
    gchar *path = write_tmp_file ("future.enc", bytes, sizeof bytes, &dir);

    assert_load_rejected (path, DEFAULT_MEMLOCK_VALUE, generic_error_gquark ());
    cleanup_tmp_file (dir, path);
}

static void
test_truncated_v3_header_rejected (void)
{
    guint8 bytes[DB_HEADER_NAME_LEN + sizeof (gint32) + 1] = {0};
    memcpy (bytes, DB_HEADER_NAME, DB_HEADER_NAME_LEN);
    write_be32_test (bytes + DB_HEADER_NAME_LEN, DB_VERSION);

    gchar *dir = NULL;
    gchar *path = write_tmp_file ("truncated-v3.enc", bytes, sizeof bytes, &dir);

    assert_load_rejected (path, DEFAULT_MEMLOCK_VALUE, generic_error_gquark ());
    cleanup_tmp_file (dir, path);
}

static void
test_truncated_v3_tag_rejected (void)
{
    guint8 bytes[DB_V3_HEADER_SIZE + TAG_SIZE - 1] = {0};
    fill_v3_header (bytes, ARGON2ID_MIN_ITER, ARGON2ID_MIN_MC, ARGON2ID_MIN_PARAL);

    gchar *dir = NULL;
    gchar *path = write_tmp_file ("truncated-tag.enc", bytes, sizeof bytes, &dir);

    assert_load_rejected (path, DEFAULT_MEMLOCK_VALUE, generic_error_gquark ());
    cleanup_tmp_file (dir, path);
}

static void
test_argon_bounds_rejected (void)
{
    guint8 bytes[DB_V3_HEADER_SIZE + TAG_SIZE] = {0};
    fill_v3_header (bytes, ARGON2ID_MAX_ITER + 1, ARGON2ID_MIN_MC, ARGON2ID_MIN_PARAL);

    gchar *dir = NULL;
    gchar *path = write_tmp_file ("bad-argon.enc", bytes, sizeof bytes, &dir);

    assert_load_rejected (path, DEFAULT_MEMLOCK_VALUE, generic_error_gquark ());
    cleanup_tmp_file (dir, path);
}

static void
test_payload_above_cap_rejected (void)
{
    guint8 bytes[64] = {0};

    gchar *dir = NULL;
    gchar *path = write_tmp_file ("too-large.enc", bytes, sizeof bytes, &dir);

    assert_load_rejected (path, 1, file_too_big_gquark ());
    cleanup_tmp_file (dir, path);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    gchar *init_err = init_libs (DEFAULT_MEMLOCK_VALUE);
    g_assert_null (init_err);

    g_test_add_func ("/malformed-db/truncated-header", test_truncated_header_rejected);
    g_test_add_func ("/malformed-db/future-version", test_future_version_rejected);
    g_test_add_func ("/malformed-db/truncated-v3-header", test_truncated_v3_header_rejected);
    g_test_add_func ("/malformed-db/truncated-v3-tag", test_truncated_v3_tag_rejected);
    g_test_add_func ("/malformed-db/argon-bounds", test_argon_bounds_rejected);
    g_test_add_func ("/malformed-db/payload-above-cap", test_payload_above_cap_rejected);

    return g_test_run ();
}
