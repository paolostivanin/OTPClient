#include <glib.h>


GQuark
missing_file_gquark (void)
{
    return g_quark_from_static_string ("missing_file");
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
file_too_big_gquark (void)
{
    return g_quark_from_static_string ("file_too_big");
}


GQuark
generic_error_gquark (void)
{
    return g_quark_from_static_string ("generic_error");
}


GQuark
memlock_error_gquark (void)
{
    return g_quark_from_static_string ("memlock_error");
}