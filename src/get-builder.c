#include <gtk/gtk.h>
#include "version.h"

GtkBuilder *
get_builder_from_partial_path (const gchar *partial_path)
{
    const gchar *prefix;
#ifndef USE_FLATPAK_APP_FOLDER
    // cmake trims the last '/', so we have to manually add it later on
    prefix = INSTALL_PREFIX;
#else
    prefix = "/app";
#endif
    gchar *path = g_strconcat (prefix, "/", partial_path, NULL);

    GtkBuilder *builder = gtk_builder_new_from_file (path);

    g_free (path);

    return builder;
}
