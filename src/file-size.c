#include <gio/gio.h>


goffset
get_file_size (const gchar *file_path)
{
    GError *error = NULL;
    GFileQueryInfoFlags flags = G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS;

    GFile *file = g_file_new_for_path (file_path);
    GFileInfo *info = g_file_query_info (G_FILE(file), "standard::*", flags, NULL, &error);
    if (info == NULL) {
        g_printerr ("%s\n", error->message);
        g_clear_error (&error);
        return -1;
    }
    goffset file_size = g_file_info_get_size (info);

    g_object_unref (file);

    return file_size;
}
