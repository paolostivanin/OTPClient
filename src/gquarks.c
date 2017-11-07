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


GQuark
bad_tag_gquark (void)
{
    return g_quark_from_static_string ("bad_tag");
}


GQuark
key_deriv_gquark (void)
{
    return g_quark_from_static_string ("key_deriv");
}


GQuark
generic_error_gquark (void)
{
    return g_quark_from_static_string ("generic_error");
}