#pragma once

#include <glib.h>
#include <jansson.h>
#include <gcrypt.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define LOW_MEMLOCK_VALUE    65536 //64KB
#define MEMLOCK_VALUE     67108864 //64MB

#define ANDOTP                 100
#define AUTHPRO                101

#define AUTHPRO_IV              12
#define AUTHPRO_SALT_TAG        16

#define ANDOTP_IV_SALT          12
#define ANDOTP_TAG              16

gint32      get_max_file_size_from_memlock  (void);

gchar      *init_libs                       (gint32          max_file_size);

gint        get_algo_int_from_str           (const gchar    *algo);

guint32     jenkins_one_at_a_time_hash      (const gchar    *key,
                                             gsize           len);

guint32     json_object_get_hash            (json_t *obj);

gchar      *secure_strdup                   (const gchar    *src);

gchar      *g_trim_whitespace               (const gchar    *str);

guchar     *hexstr_to_bytes                 (const gchar    *hexstr);

gchar      *bytes_to_hexstr                 (const guchar   *data,
                                             size_t          datalen);

GSList     *decode_migration_data           (const gchar    *encoded_uri);

gchar      *g_uri_unescape_string_secure    (const gchar    *escaped_string,
                                             const gchar    *illegal_characters);

guchar     *g_base64_decode_secure          (const gchar    *text,
                                             gsize          *out_len);

gcry_cipher_hd_t open_cipher_and_set_data   (guchar         *derived_key,
                                             guchar         *iv,
                                             gsize           iv_len);

GKeyFile   *get_kf_ptr                      (void);

guchar     *get_andotp_derived_key          (const gchar    *password,
                                             const guchar   *salt,
                                             guint32         iterations);

guchar     *get_authpro_derived_key         (const gchar    *password,
                                             const guchar   *salt);

gchar      *get_data_from_encrypted_backup  (const gchar    *path,
                                             const gchar    *password,
                                             gint32          max_file_size,
                                             gint32          provider,
                                             guint32         andotp_be_iterations,
                                             GFile          *in_file,
                                             GFileInputStream  *in_stream,
                                             GError        **err);

G_END_DECLS
