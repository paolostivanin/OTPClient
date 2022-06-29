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
    for (hash = i = 0; i < len; ++i) {
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

gchar *
secure_strdup (const gchar *src)
{
    gchar *sec_buf = gcry_calloc_secure (strlen (src) + 1, 1);
    memcpy (sec_buf, src, strlen (src) + 1);

    return sec_buf;
}


gchar *
g_trim_whitespace (const gchar *str)
{
    if (g_utf8_strlen (str, -1) == 0) {
        return NULL;
    }
    gchar *sec_buf = gcry_calloc_secure (strlen (str) + 1, 1);
    int pos = 0;
    for (int i = 0; str[i]; i++) {
        if (str[i] != ' ') {
            sec_buf[pos++] = str[i];
        }
    }
    sec_buf[pos] = '\0';
    gcry_realloc (sec_buf, g_utf8_strlen(sec_buf, -1) + 1);

    return sec_buf;
}


guchar *
hexstr_to_bytes (const gchar *hexstr)
{
    size_t len = strlen (hexstr);
    size_t final_len = len / 2;
    guchar *chrs = (guchar *)g_malloc((final_len+1) * sizeof(*chrs));
    for (size_t i = 0, j = 0; j < final_len; i += 2, j++)
        chrs[j] = (hexstr[i] % 32 + 9) % 25 * 16 + (hexstr[i+1] % 32 + 9) % 25;
    chrs[final_len] = '\0';
    return chrs;
}


gchar *
bytes_to_hexstr (const guchar *data, size_t datalen)
{
    gchar hex_str[]= "0123456789abcdef";

    gchar *result = g_malloc0(datalen * 2 + 1);
    if (result == NULL) {
        g_printerr ("Error while allocating memory for bytes_to_hexstr.\n");
        return result;
    }

    for (guint i = 0; i < datalen; i++)
    {
        result[i * 2 + 0] = hex_str[(data[i] >> 4) & 0x0F];
        result[i * 2 + 1] = hex_str[(data[i]     ) & 0x0F];
    }

    result[datalen * 2] = 0;

    return result;
}
