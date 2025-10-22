#include <gio/gio.h>

// Returns -1 on error, 0 if file doesn't exist, or the file size on success.
goffset
get_file_size (const gchar *file_path)
{
    if (file_path == NULL || !g_file_test (file_path, G_FILE_TEST_EXISTS)) {
        return 0;
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
