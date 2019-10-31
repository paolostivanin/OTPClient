#include <glib.h>
#include <sys/resource.h>
#include <cotp.h>
#include "gcrypt.h"
#include "jansson.h"

gint32
get_max_file_size_from_memlock (void)
{
    struct rlimit r;
    if (getrlimit (RLIMIT_MEMLOCK, &r) == -1) {
        // couldn't get memlock limit, so falling back to a default, low value
        g_print ("[WARNING] your OS's memlock limit may be too low for you (64000 bytes). Please have a look at https://github.com/paolostivanin/OTPClient#limitations\n");
        return 64000;
    } else {
        if (r.rlim_cur == -1 || r.rlim_cur > 4194304) {
            // memlock is either unlimited or bigger than needed
            return 4194304;
        } else {
            // memlock is less than 4 MB
            g_print ("[WARNING] your OS's memlock limit may be too low for you (%d bytes). Please have a look at https://github.com/paolostivanin/OTPClient#limitations\n", (gint32)r.rlim_cur);
            return (gint32)r.rlim_cur;
        }
    }
}


gchar *
init_libs (gint32 max_file_size)
{
    if (!gcry_check_version ("1.6.0")) {
        return g_strdup ("The required version of GCrypt is 1.6.0 or greater.");
    }

    if (gcry_control (GCRYCTL_INIT_SECMEM, max_file_size, 0)) {
        return g_strdup ("Couldn't initialize secure memory.\n");
    }
    gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);

    json_set_alloc_funcs (gcry_malloc_secure, gcry_free);

    return NULL;
}


gint
get_algo_int_from_str (const gchar *algo)
{
    gint algo;
    if (g_strcmp0 (algo, "SHA1") == 0) {
        algo = SHA1;
    } else if (g_strcmp0 (algo, "SHA256") == 0) {
        algo = SHA256;
    } else {
        algo = SHA512;
    }

    return algo;
}