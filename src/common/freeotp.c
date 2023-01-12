#include <glib.h>
#include <gcrypt.h>
#include <jansson.h>
#include <time.h>
#include "../file-size.h"
#include "../parse-uri.h"


GSList *
get_freeotpplus_data (const gchar     *path,
                      GError         **err)
{
    GSList *otps = NULL;
    goffset fs = get_file_size (path);
    if (fs < 10) {
        g_printerr ("Couldn't get the file size (file doesn't exit or wrong file selected\n");
        return NULL;
    }
    gchar *sec_buf = gcry_calloc_secure (fs, 1);
    if (!g_file_get_contents (path, &sec_buf, NULL, err)) {
        g_printerr("Couldn't read into memory the freeotp txt file\n");
        gcry_free (sec_buf);
        return NULL;
    }

    set_otps_from_uris (sec_buf, &otps);

    gcry_free (sec_buf);

    return otps;
}


gchar *
export_freeotpplus (const gchar *export_path,
                    json_t      *json_db_data)
{
    json_t *db_obj;
    gsize index;

    FILE *fp = fopen (export_path, "w");
    if (fp == NULL) {
        return g_strdup ("couldn't create the file object");
    }

    json_array_foreach (json_db_data, index, db_obj) {
        gchar *uri = get_otpauth_uri (NULL, db_obj);
        fwrite (uri, g_utf8_strlen (uri, -1), 1, fp);
        g_free (uri);
    }

    fclose (fp);

    return NULL;
}
