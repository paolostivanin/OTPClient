#include <glib.h>
#include <gio/gio.h>

static GSList *get_otps_from_encrypted_authpro_backup (const gchar       *path,
                                                       const gchar       *password,
                                                       gint32             max_file_size,
                                                       GFile             *in_file,
                                                       GFileInputStream  *in_stream,
                                                       GError           **err);


GSList *
get_authpro_data (const gchar  *path,
                  const gchar  *password,
                  gint32        max_file_size,
                  GError      **err)
{
    GFile *in_file = g_file_new_for_path(path);
    GFileInputStream *in_stream = g_file_read(in_file, NULL, err);
    if (*err != NULL) {
        g_object_unref(in_file);
        return NULL;
    }
    return get_otps_from_encrypted_authpro_backup (path, password, max_file_size, in_file, in_stream, err);
}


static GSList *
get_otps_from_encrypted_authpro_backup (const gchar       *path,
                                        const gchar       *password,
                                        gint32             max_file_size,
                                        GFile             *in_file,
                                        GFileInputStream  *in_stream,
                                        GError           **err)
{
    return NULL;
}