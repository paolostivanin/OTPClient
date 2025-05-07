#include <gio/gio.h>

goffset
get_file_size (const gchar *file_path)
{
    GError *error = NULL;

    if (file_path == NULL || !g_file_test (file_path, G_FILE_TEST_EXISTS)) {
        return 0;
    }

    GFile *file = g_file_new_for_path (file_path);
    GFileInfo *info = g_file_query_info (G_FILE(file), "standard::*", G_FILE_QUERY_INFO_NONE, NULL, &error);
    if (info == NULL) {
        g_printerr ("%s\n", error->message);
        g_clear_error (&error);
        return -1;
    }
    goffset file_size = g_file_info_get_size (info);

    g_object_unref (file);

    return file_size;
}
