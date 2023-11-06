#include <gtk-4.0/gtk/gtk.h>
#include <glib/gi18n.h>
#include "otpclient-application.h"
#include "version.h"

gint
main (gint    argc,
      gchar **argv)
{
    g_autoptr (OTPClientApplication)app = NULL;

    bindtextdomain (GETTEXT_PACKAGE, LOCALE_DIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    g_set_application_name (_(PROJECT_NAME));

    app = otpclient_application_new ();
    g_application_set_default (G_APPLICATION(app));

    return g_application_run (G_APPLICATION(app), argc, argv);
}
