#include <gtk/gtk.h>
#include <sys/resource.h>
#include "otpclient.h"


static gint64 get_current_memlock_limit (void);


gint
main (gint    argc,
      gchar **argv)
{
    GtkApplication *app;
    gint status;

    app = gtk_application_new ("com.github.paolostivanin.OTPClient", G_APPLICATION_FLAGS_NONE);
    g_set_application_name (APP_NAME);
    g_set_prgname (APP_NAME);

    gint64 limit = get_current_memlock_limit ();
    g_signal_connect (app, "activate", G_CALLBACK (activate), (gpointer) limit);

    status = g_application_run (G_APPLICATION (app), argc, argv);

    g_object_unref (app);

    return status;
}


static gint64
get_current_memlock_limit ()
{
    struct rlimit r;
    if (getrlimit (RLIMIT_MEMLOCK, &r) == -1) {
        g_printerr ("Couldn't get memlock limits.\n");
        return -5;
    } else {
        return r.rlim_cur;
    }
}
