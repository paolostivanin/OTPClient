#include <glib.h>
#include <gcrypt.h>
#include <jansson.h>
#include <time.h>
#include "../file-size.h"
#include "../parse-uri.h"
#include "../gquarks.h"


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

    GError *err = NULL;
    GFile *out_gfile = g_file_new_for_path (export_path);
    GFileOutputStream *out_stream = g_file_replace (out_gfile, NULL, FALSE, G_FILE_CREATE_REPLACE_DESTINATION | G_FILE_CREATE_PRIVATE, NULL, &err);
    if (err == NULL) {
        json_array_foreach (json_db_data, index, db_obj) {
            gchar *uri = get_otpauth_uri (NULL, db_obj);
            if (g_output_stream_write (G_OUTPUT_STREAM(out_stream), uri, g_utf8_strlen (uri, -1) + 1, NULL, &err) == -1) {
                g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "couldn't dump json data to file");
            }
            g_free (uri);
        }
    } else {
        g_set_error (&err, generic_error_gquark (), GENERIC_ERRCODE, "couldn't create the file object");
    }

    g_object_unref (out_stream);
    g_object_unref (out_gfile);

    return (err != NULL ? g_strdup (err->message) : NULL);
}
