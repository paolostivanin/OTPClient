#include <glib.h>
#include <gio/gio.h>
#include <jansson.h>
#include <time.h>
#include "gquarks.h"
#include "parse-uri.h"


GSList *
get_freeotpplus_data (const gchar  *path,
                      gint32        max_file_size,
                      GError      **err)
{
    return get_otpauth_data (path, max_file_size, err);
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
            gchar *uri = get_otpauth_uri (db_obj);
            if (g_output_stream_write (G_OUTPUT_STREAM(out_stream), uri, g_utf8_strlen (uri, -1), NULL, &err) == -1) {
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
