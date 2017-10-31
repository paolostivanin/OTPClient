#include <glib.h>


GQuark
missing_file_gquark (void)
{
    return g_quark_from_static_string ("missing_file");
}


GQuark
invalid_input_gquark (void)
{
    return g_quark_from_static_string ("invalid_input");
}