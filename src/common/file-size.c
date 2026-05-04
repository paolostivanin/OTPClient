#include <gio/gio.h>

/* Returns -1 on every error (NULL path, missing file, query failure). On
 * success returns the actual file size, which may legitimately be 0 for an
 * empty file — callers can distinguish "missing" from "empty" by checking
 * for -1 instead of 0. */
goffset
get_file_size (const gchar *file_path)
{
    if (file_path == NULL || !g_file_test (file_path, G_FILE_TEST_EXISTS)) {
        return -1;
    }

    GError *error = NULL;
    GFile *file = g_file_new_for_path (file_path);
    if (file == NULL) {
        return -1;
    }

    // Query only the size to avoid unnecessary I/O on other attributes
    GFileInfo *info = g_file_query_info (file, "standard::size", G_FILE_QUERY_INFO_NONE, NULL, &error);
    if (info == NULL) {
        if (error != NULL) {
            g_printerr ("Failed to query file size: %s\n", error->message);
            g_clear_error (&error);
        }
        g_object_unref (file);
        return -1;
    }

    goffset file_size = g_file_info_get_size (info);

    g_object_unref (info);
    g_object_unref (file);

    return file_size;
}
