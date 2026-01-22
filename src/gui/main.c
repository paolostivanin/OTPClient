#include <gtk/gtk.h>
#include <glib/gi18n.h>
#include "otpclient-application.h"
#include "version.h"

gint
main (gint    argc,
      gchar **argv)
{
    bindtextdomain (GETTEXT_PACKAGE, LOCALE_DIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    OtpclientApplication *app = otpclient_application_new ();
    g_set_application_name (PROJECT_NAME);

    gint status = g_application_run (G_APPLICATION (app), argc, argv);

    g_object_unref (app);

    return status;
}
