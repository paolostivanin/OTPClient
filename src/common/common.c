#include <glib.h>
#include <sys/resource.h>
#include <cotp.h>
#include "gcrypt.h"
#include "jansson.h"

gint32
get_max_file_size_from_memlock (void)
{
    const gchar *link = "https://github.com/paolostivanin/OTPClient/wiki/Secure-Memory-Limitations";
    struct rlimit r;
    if (getrlimit (RLIMIT_MEMLOCK, &r) == -1) {
        // couldn't get memlock limit, so falling back to a default, low value
        g_print ("[WARNING] your OS's memlock limit may be too low for you (64000 bytes). Please have a look at %s\n", link);
        return 64000;
    } else {
        if (r.rlim_cur == -1 || r.rlim_cur > 4194304) {
            // memlock is either unlimited or bigger than needed
            return 4194304;
        } else {
            // memlock is less than 4 MB
            g_print ("[WARNING] your OS's memlock limit may be too low for you (%d bytes). Please have a look at %s\n", (gint32)r.rlim_cur, link);
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
    gint algo_int;
    if (g_strcmp0 (algo, "SHA1") == 0) {
        algo_int = SHA1;
    } else if (g_strcmp0 (algo, "SHA256") == 0) {
        algo_int = SHA256;
    } else {
        algo_int = SHA512;
    }

    return algo_int;
}


guint32
jenkins_one_at_a_time_hash (const gchar *key, gsize len)
{
    guint32 hash, i;
    for(hash = i = 0; i < len; ++i) {
        hash += key[i];
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }
    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);

    return hash;
}


guint32
json_object_get_hash (json_t *obj)
{
    const gchar *key;
    json_t *value;
    gchar *tmp_string = gcry_calloc_secure (256, 1);
    json_object_foreach (obj, key, value) {
        if (g_strcmp0 (key, "period") == 0 || g_strcmp0 (key, "counter") == 0 || g_strcmp0 (key, "digits") == 0) {
            json_int_t v = json_integer_value (value);
            g_snprintf (tmp_string + strlen (tmp_string), 256, "%ld", (gint64) v);
        } else {
            g_strlcat (tmp_string, json_string_value (value), 256);
        }
    }

    guint32 hash = jenkins_one_at_a_time_hash (tmp_string, strlen (tmp_string) + 1);

    gcry_free (tmp_string);

    return hash;
}
