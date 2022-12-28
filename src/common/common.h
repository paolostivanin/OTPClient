#pragma once

#include <glib.h>
#include <jansson.h>
#include <gcrypt.h>

G_BEGIN_DECLS

#if GLIB_CHECK_VERSION(2, 68, 0)
    #define g_memdupX g_memdup2
#else
    #define g_memdupX g_memdup
#endif

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

guint       g_string_replace_backported     (GString        *string,
                                             const gchar    *find,
                                             const gchar    *replace,
                                             guint           limit);

gchar      *g_uri_unescape_string_secure    (const gchar    *escaped_string,
                                             const gchar    *illegal_characters);

guchar     *g_base64_decode_secure          (const gchar    *text,
                                             gsize          *out_len);

gcry_cipher_hd_t open_cipher_and_set_data   (guchar         *derived_key,
                                             guchar         *iv,
                                             gsize           iv_len);

G_END_DECLS
