#include <gtk/gtk.h>


GtkBuilder *
get_builder_from_partial_path (const gchar *partial_path)
{
    const gchar *prefix;
#ifndef USE_FLATPAK_APP_FOLDER
    if (g_file_test (g_strconcat ("/usr/", partial_path, NULL), G_FILE_TEST_EXISTS)) {
        prefix = "/usr/";
    } else if (g_file_test (g_strconcat ("/usr/local/", partial_path, NULL), G_FILE_TEST_EXISTS)) {
        prefix = "/usr/local/";
    } else {
        return NULL;
    }
#else
    prefix = "/app/";
#endif
    gchar *path = g_strconcat (prefix, partial_path, NULL);

    GtkBuilder *builder = gtk_builder_new_from_file (path);

    g_free (path);

    return builder;
}