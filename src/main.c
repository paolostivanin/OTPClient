#include <gtk/gtk.h>
#include <sys/mman.h>
#include "otpclient.h"


gint
main (gint    argc,
      gchar **argv)
{
    mlockall (MCL_CURRENT | MCL_FUTURE);

    GtkApplication *app;
    gint status;

    app = gtk_application_new ("com.github.paolostivanin.OTPClient", G_APPLICATION_FLAGS_NONE);
    g_set_application_name (APP_NAME);
    g_set_prgname (APP_NAME);
    g_signal_connect (app, "activate", G_CALLBACK (activate), NULL);

    status = g_application_run (G_APPLICATION (app), argc, argv);

    g_object_unref (app);

    return status;
}
