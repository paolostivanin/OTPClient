#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

GSList *get_authplus_data       (const gchar     *zip_path,
                                 const gchar     *password,
                                 gint32           max_file_size,
                                 GError         **err);

GSList *get_andotp_data         (const gchar     *path,
                                 const gchar     *password,
                                 gint32           max_file_size,
                                 gboolean         encrypted,
                                 GError         **err);

GSList *get_freeotpplus_data    (const gchar     *path,
                                 GError         **err);

GSList *get_aegis_data          (const gchar     *path,
                                 const gchar     *password,
                                 gint32           max_file_size,
                                 gboolean         encrypted,
                                 GError         **err);

G_END_DECLS
