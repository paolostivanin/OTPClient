#include <glib.h>
#include <zip.h>
#include <gcrypt.h>
#include "gquarks.h"
#include "parse-uri.h"


GSList *
get_authplus_data (const gchar   *zip_path,
                   const gchar   *password,
                   gint32         max_file_size,
                   GError       **err)
{
    zip_t *zip_file;
    struct zip_file *zf;
    struct zip_stat sb;

    int zip_err;
    if ((zip_file = zip_open (zip_path, ZIP_RDONLY, &zip_err)) == NULL) {
        zip_error_t error;
        zip_error_init_with_code (&error, zip_err);
        gchar *msg = g_strdup_printf ("Couldn't open zip file '%s': %s", zip_path, zip_error_strerror (&error));
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "%s", msg);
        g_free (msg);
        zip_error_fini(&error);
        return NULL;
    }

    zip_set_default_password (zip_file, password);

    if (zip_stat (zip_file, "Accounts.txt", 0, &sb) < 0) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "%s", "zip_stat failed");
        zip_discard (zip_file);
        return NULL;
    }

    if (sb.size > max_file_size) {
        g_set_error (err, file_too_big_gquark (), FILE_TOO_BIG, "File is too big");
        zip_discard (zip_file);
        return NULL;
    }
    gchar *sec_buf = gcry_calloc_secure (sb.size, 1);
    if (sec_buf == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "Couldn't allocate secure memory");
        zip_discard (zip_file);
        return NULL;
    }
    zf = zip_fopen (zip_file, "Accounts.txt", 0);
    if (zf == NULL) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "%s", "zip_fopen failed");
        zip_discard (zip_file);
        return NULL;
    }

    zip_int64_t len = zip_fread (zf, sec_buf, sb.size);
    if (len < 0) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE, "%s", "zip_fread failed");
        zip_fclose (zf);
        zip_discard (zip_file);
        return NULL;
    }

    GSList *otps = NULL;
    set_otps_from_uris (sec_buf, &otps);

    zip_fclose (zf);

    zip_discard (zip_file);

    gcry_free (sec_buf);

    return otps;
}