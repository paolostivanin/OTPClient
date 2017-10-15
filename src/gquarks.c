#include <glib.h>


GQuark
missing_file_gquark ()
{
    return g_quark_from_static_string ("missing_file");
}


GQuark
invalid_input_gquark ()
{
    return g_quark_from_static_string ("invalid_input");
}