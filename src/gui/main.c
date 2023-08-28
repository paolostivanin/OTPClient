#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include "otpclient.h"
#include "version.h"

gint
main (gint    argc,
      gchar **argv)
{
    bindtextdomain (GETTEXT_PACKAGE, LOCALE_DIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    GApplicationFlags flags = G_APPLICATION_DEFAULT_FLAGS;
    GtkApplication *app = gtk_application_new ("com.github.paolostivanin.OTPClient", flags);
    g_set_application_name (PROJECT_NAME);

    g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);

    gint status = g_application_run (G_APPLICATION (app), argc, argv);

    g_object_unref (app);

    return status;
}
