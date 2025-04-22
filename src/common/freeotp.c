#include <glib.h>
#include <gio/gio.h>
#include <jansson.h>
#include <time.h>
#include <glib/gi18n.h>

#include "common.h"
#include "gquarks.h"
#include "parse-uri.h"


GSList *
get_freeotpplus_data (const gchar  *path,
                      gint32        max_file_size,
                      gsize         db_size,
                      GError      **err)
{
    if (!is_secmem_available (db_size * SECMEM_REQUIRED_MULTIPLIER, err)) {
        g_autofree gchar *msg = g_strdup_printf (_(
            "Your system's secure memory limit is not enough to securely import the data.\n"
            "You need to increase your system's memlock limit by following the instructions on our "
            "<a href=\"https://github.com/paolostivanin/OTPClient/wiki/Secure-Memory-Limitations\">secure memory wiki page</a>.\n"
            "This requires administrator privileges and is a system-wide setting that OTPClient cannot change automatically."
        ));
        g_set_error (err, secmem_alloc_error_gquark (), NO_SECMEM_AVAIL_ERRCODE, "%s", msg);
        return NULL;
    }
    return get_otpauth_data (path, max_file_size, err);
}


gchar *
export_freeotpplus (const gchar *export_path,
                    json_t      *json_db_data)
{
    gsize db_size = json_dumpb (json_db_data, NULL, 0, 0);
    if (!is_secmem_available (db_size * SECMEM_REQUIRED_MULTIPLIER, NULL)) {
        return g_strdup_printf (_(
            "Your system's secure memory limit is not enough to securely export the database.\n"
            "You need to increase your system's memlock limit by following the instructions on our "
            "<a href=\"https://github.com/paolostivanin/OTPClient/wiki/Secure-Memory-Limitations\">secure memory wiki page</a>.\n"
            "This requires administrator privileges and is a system-wide setting that OTPClient cannot change automatically."
        ));
    }

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
