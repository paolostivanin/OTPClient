#pragma once

#include <glib.h>
#include "import-diagnostics.h"

G_BEGIN_DECLS

GSList *get_freeotpplus_data (const gchar     *path,
                              gint32           max_file_size,
                              gsize            db_size,
                              GError         **err);

GSList *get_aegis_data       (const gchar     *path,
                              const gchar     *password,
                              gint32           max_file_size,
                              gsize            db_size,
                              GError         **err);

GSList *get_authpro_data     (const gchar     *path,
                              const gchar     *password,
                              gint32           max_file_size,
                              gsize            db_size,
                              GError         **err);

GSList *get_twofas_data      (const gchar     *path,
                              const gchar     *password,
                              gsize            db_size,
                              GError         **err);

GSList *get_freeotpplus_data_full (const gchar     *path,
                              gint32           max_file_size,
                              gsize            db_size,
                              OtpImportDiagnostics *diagnostics, GError         **err);

GSList *get_aegis_data_full       (const gchar     *path,
                              const gchar     *password,
                              gint32           max_file_size,
                              gsize            db_size,
                              OtpImportDiagnostics *diagnostics, GError         **err);

GSList *get_authpro_data_full     (const gchar     *path,
                              const gchar     *password,
                              gint32           max_file_size,
                              gsize            db_size,
                              OtpImportDiagnostics *diagnostics, GError         **err);

GSList *get_twofas_data_full      (const gchar     *path,
                              const gchar     *password,
                              gsize            db_size,
                              OtpImportDiagnostics *diagnostics, GError         **err);


G_END_DECLS
