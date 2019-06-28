#include <glib.h>
#include "help.h"


gint
main (gint argc, gchar **argv)
{
    if (show_help (argv[0], argv[1]) == -1 || argc == 0) {
        return -1;
    }

    return 0;
}