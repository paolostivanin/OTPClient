#pragma once

#include <glib.h>
#include <gio/gio.h>
#include <jansson.h>
#include <gcrypt.h>

G_BEGIN_DECLS

#define MEMLOCK_ERR                      1
#define MEMLOCK_OK                       2
#define MEMLOCK_TOO_LOW                  3
#define DEFAULT_MEMLOCK_VALUE     67108864 // 64 MiB
#define SECMEM_HEADROOM_VALUE      1048576 // 1 MiB left unlocked below RLIMIT_MEMLOCK so GTK/libadwaita can still mlock the password-entry buffer
#define MIN_SECMEM_POOL_VALUE      1048576 // 1 MiB floor for our own pool when the limit is too low to reserve headroom
#define SECMEM_SIZE_THRESHOLD_RATIO   0.80
#define SECMEM_REQUIRED_MULTIPLIER       3

#define AUTHPRO                101

#define AUTHPRO_IV              12
#define AUTHPRO_SALT_TAG        16

typedef struct otp_object_t {
    gchar *type;

    gchar *algo;

    guint32 digits;

    union {
        guint32 period;
        guint64 counter;
    };

    gchar *account_name;

    gchar *issuer;

    gchar *secret;

    gchar *group;
} otp_t;


gint32            set_memlock_value (gint32             *memlock_value);

gint32            secmem_pool_from_limits          (guint64             memlock_budget,
                                                  gint32             *memlock_value);

gchar            *init_libs                      (gint32              max_file_size);

gint              get_algo_int_from_str          (const gchar        *algo);

guchar           *hexstr_to_bytes                (const gchar        *hexstr);

guchar           *hexstr_to_bytes_exact          (const gchar        *hexstr,
                                                  gsize               expected_len);

gchar            *bytes_to_hexstr                (const guchar       *data,
                                                  size_t              datalen);

gcry_cipher_hd_t  open_cipher_and_set_data       (guchar             *derived_key,
                                                  guchar             *iv,
                                                  gsize               iv_len);

gchar            *secure_strdup                  (const gchar        *src);
void              sensitive_free                 (gchar              *value);
void              sensitive_g_free               (gchar              *value);
void              sensitive_secure_free          (gchar              *value);

gchar            *get_data_from_encrypted_backup (const gchar        *path,
                                                  const gchar        *password,
                                                  gint32              max_file_size,
                                                  gint32              provider,
                                                  GFile              *in_file,
                                                  GFileInputStream   *in_stream,
                                                  GError            **err);

guchar           *get_authpro_derived_key        (const gchar        *password,
                                                  const guchar       *salt);

guint32           json_object_get_hash           (json_t             *obj);

void              free_otps_gslist               (GSList             *otps,
                                                  guint               list_len);

json_t           *build_json_obj                 (const gchar        *type,
                                                  const gchar        *acc_label,
                                                  const gchar        *acc_iss,
                                                  const gchar        *acc_key,
                                                  guint               digits,
                                                  const gchar        *algo,
                                                  guint               period,
                                                  guint64             ctr,
                                                  const gchar        *group);

json_t           *get_json_root                  (const gchar        *path);

void              json_free                      (gpointer            data);

GKeyFile         *get_kf_ptr                     (void);

gboolean          is_secmem_available            (gsize               required_size,
                                                  GError            **err);

gboolean          output_stream_write_all_exact   (GOutputStream      *stream,
                                                   const void         *buffer,
                                                   gsize               count,
                                                   GError            **err);

/* Open a path with O_NOFOLLOW, fstat to confirm it's a regular file (not a
 * symlink, directory, or special file), and return the open fd. Caller is
 * responsible for closing it. To eliminate the close-then-reopen TOCTOU,
 * downstream readers should access the same inode via "/proc/self/fd/N"
 * rather than opening `path` again. Returns -1 on error. */
int               path_open_safe_regular_file    (const gchar        *path,
                                                  GError            **err);

G_END_DECLS
