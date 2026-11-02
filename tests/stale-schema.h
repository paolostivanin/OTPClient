#pragma once

/* A compiled settings schema from an older release, for tests that need GLib to
 * find one ahead of the current schema (#475). Needs OTPCLIENT_SCHEMA_XML and
 * GLIB_COMPILE_SCHEMAS_PATH from CMake; without the compiler, callers skip. */

#include <glib.h>
#include <glib/gstdio.h>
#include <string.h>

#define STALE_SCHEMA_FILE "com.github.paolostivanin.OTPClient.gschema.xml"

#ifdef GLIB_COMPILE_SCHEMAS_PATH
/* Compile the checkout's schema with one key cut out, which is what the schema
 * of the release before that key was added looks like, into a new temporary
 * directory. Free with stale_schema_dir_free. */
static inline gchar *
stale_schema_dir_new (const gchar *dropped_key)
{
    g_autoptr (GError) err = NULL;
    g_autofree gchar *xml = NULL;
    g_assert_true (g_file_get_contents (OTPCLIENT_SCHEMA_XML, &xml, NULL, &err));
    g_assert_no_error (err);

    g_autofree gchar *open_tag = g_strdup_printf ("<key name=\"%s\"", dropped_key);
    gchar *start = strstr (xml, open_tag);
    g_assert_nonnull (start);
    gchar *end = strstr (start, "</key>");
    g_assert_nonnull (end);
    end += strlen ("</key>");
    memmove (start, end, strlen (end) + 1);

    gchar *dir = g_dir_make_tmp ("otpclient-stale-schema-XXXXXX", &err);
    g_assert_no_error (err);
    g_autofree gchar *path = g_build_filename (dir, STALE_SCHEMA_FILE, NULL);
    g_assert_true (g_file_set_contents (path, xml, -1, &err));
    g_assert_no_error (err);

    const gchar *argv[] = { GLIB_COMPILE_SCHEMAS_PATH, dir, NULL };
    gint status = 0;
    g_assert_true (g_spawn_sync (NULL, (gchar **) argv, NULL, G_SPAWN_DEFAULT,
                                 NULL, NULL, NULL, NULL, &status, &err));
    g_assert_no_error (err);
    g_assert_true (g_spawn_check_wait_status (status, &err));
    g_assert_no_error (err);
    return dir;
}

static inline void
stale_schema_dir_free (gchar *dir)
{
    const gchar *files[] = { STALE_SCHEMA_FILE, "gschemas.compiled" };
    for (gsize i = 0; i < G_N_ELEMENTS (files); i++) {
        g_autofree gchar *path = g_build_filename (dir, files[i], NULL);
        g_unlink (path);
    }
    g_rmdir (dir);
    g_free (dir);
}

/* Point GSETTINGS_SCHEMA_DIR at dir for a g_test_trap_subprocess child, which
 * builds its default schema source from scratch, then put it back. */
static inline void
stale_schema_trap_subprocess (const gchar *dir, guint64 timeout)
{
    g_autofree gchar *saved = g_strdup (g_getenv ("GSETTINGS_SCHEMA_DIR"));
    g_setenv ("GSETTINGS_SCHEMA_DIR", dir, TRUE);
    g_test_trap_subprocess (NULL, timeout, 0);
    if (saved != NULL)
        g_setenv ("GSETTINGS_SCHEMA_DIR", saved, TRUE);
    else
        g_unsetenv ("GSETTINGS_SCHEMA_DIR");
}
#endif
