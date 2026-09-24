#define _DEFAULT_SOURCE
#include <gio/gio.h>
#include <glib.h>
#include <jansson.h>
#include <libsecret/secret.h>
#include <gcrypt.h>
#include <cotp.h>
#include <string.h>
#include <time.h>
#include <limits.h>

#include "../common/common.h"
#include "../common/db-common.h"
#include "../common/file-size.h"
#include "../common/otp-validation.h"
#include "../common/secret-schema.h"
#include "../common/gsettings-common.h"
#include "dbus-signatures.h"

#define KRUNNER_BUS "com.github.paolostivanin.OTPClient.KRunner"
#define KRUNNER_PATH "/com/github/paolostivanin/OTPClient/KRunner"
#define GNOME_BUS "com.github.paolostivanin.OTPClient.SearchProvider"
#define GNOME_PATH "/com/github/paolostivanin/OTPClient/SearchProvider"

static gint32 global_max_file_size = 0;

/* libgcrypt's secure-memory pool is mlocked, so a provider that is installed but
 * switched off should not pay for it. Set up on first use instead of at startup,
 * because the daemon now stays alive while disabled (see main) to answer the
 * bus rather than letting activation fail. */
static gboolean crypto_ready = FALSE;

/* Cache TTL is a fallback: file monitors invalidate the cache eagerly when a
 * DB file is touched on disk. The TTL covers the case where the on-disk DB
 * is unchanged but a HOTP counter / secret service value moved underneath us. */
#define CACHE_TTL_SECONDS 60

static GPtrArray  *cached_entries = NULL;
static gint64      cached_at = 0;
/* path (gchar*) -> GFileMonitor*. Diffed across reloads so monitors for
 * unchanged DB paths survive without a brief monitor-less window during
 * which writes would slip past the cache invalidator. */
static GHashTable *file_monitors = NULL;

/* Argon2id is ~150-300 ms per derivation, paid in the user-visible latency
 * between Activate/Run and the notification. The original design comment
 * claimed the per-db_data cached_derived_key field covered this, but the
 * cache lives on the DatabaseData that compute_otp_for_entry creates fresh
 * (and frees) every call, so the derivation actually ran every time.
 *
 * g_kdf_cache lifts the derived key out of DatabaseData so it survives across
 * activations. Each entry is invalidated eagerly by the existing GFileMonitor
 * when the DB file changes (password change -> new salt -> cache miss anyway,
 * but we drop the entry to wipe the old derived key sooner).
 *
 * Trade-off: ARGON2ID_KEYLEN bytes per active DB live in secure memory
 * between calls. The plaintext json is still wiped after each compute_otp call. */
typedef struct {
    guchar  *derived_key;             /* ARGON2ID_KEYLEN bytes, gcry_malloc_secure */
    guint8   salt[KDF_SALT_SIZE];
    guint8   pwd_hash[32];            /* SHA-256(password) */
} KdfCacheEntry;

static GHashTable *g_kdf_cache = NULL;

/* Per-sender token bucket for Activate/Run. Without it, any session-bus peer
 * can spam OTP delivery (which sends a notification carrying the live code)
 * at unlimited rate. Match queries are not rate-limited here because the
 * entries cache already absorbs them; only the OTP-yielding paths are.
 *
 * The buckets are keyed on the D-Bus sender so a local peer that knows the
 * keyword cannot drain a shared bucket and starve the real user. */
#define RATE_BUCKET_MAX     10.0
#define RATE_REFILL_PER_SEC  5.0
/* Cap on distinct senders tracked at once, so a peer that fabricates unique
 * sender names cannot grow the map without bound; entries idle longer than
 * RATE_BUCKET_TTL_US are evicted to make room. */
#define RATE_BUCKETS_MAX_SENDERS 32
#define RATE_BUCKET_TTL_US (60 * G_USEC_PER_SEC)
typedef struct {
    gdouble tokens;
    gint64 last_refill_us;
    gint64 last_seen_us;
} RateBucket;

static GHashTable *g_rate_buckets = NULL;  /* sender (owned) -> RateBucket* */
static gint64 g_last_activity_us = 0;
#define IDLE_WIPE_SECONDS 300

#define ACTIVATION_CAP_TTL_US (30 * G_USEC_PER_SEC)
typedef struct {
    gchar *id;
    gchar *sender;
    gchar *query;
    gchar *db_path;
    gchar *label;
    /* Stable token identity (issuer + label + content hash), not the
     * positional index: a DB write between query and activation can
     * reorder/remove entries, and an index would then resolve to a different
     * token. */
    gchar *token_identity;
    gsize  json_index;
    gint64 expires_at_us;
} ActivationCapability;

static GHashTable *g_activation_caps = NULL;

/* Settings changes invalidate both cached data and pending deliveries. */
static GSettings *provider_settings;
static guint64 delivery_generation;
static gboolean provider_access_allowed (void);
static gchar *g_keyword = NULL;
static gchar *g_keyword_fold = NULL;

typedef struct otp_search_entry_t {
    gchar *id;
    gchar *label;
    gchar *issuer;
    gchar *db_name;
    gchar *db_path;        /* needed to recompute OTP on Run/Activate */
    gchar *token_identity; /* issuer + label + content hash, used to re-find the token */
    gsize  json_index;     /* position of the token within the DB's JSON array */
    /* Pre-folded copies for entry_matches_terms - avoid casefolding per query. */
    gchar *label_fold;
    gchar *issuer_fold;
} OtpSearchEntry;

static void otp_search_entry_free (OtpSearchEntry *entry);
static void get_entries_async (GAsyncReadyCallback callback, gpointer user_data);
static GPtrArray *get_entries_finish (GAsyncResult *result);
static gboolean entry_matches_terms (const OtpSearchEntry *entry, gchar **terms, gsize terms_len);
static gchar *get_entry_otp_value (json_t *obj);
static gchar *compute_otp_with_password (const gchar  *db_path,
                                         const gchar  *token_identity,
                                         gsize         json_index,
                                         const gchar  *password,
                                         gchar       **out_failure);
static gchar *token_identity_from_obj (json_t *obj);
static void send_notification (const gchar *label, const gchar *otp_value);
static void send_failure_notification (const gchar *label, const gchar *reason);
static void copy_to_clipboard (GDBusConnection *conn, const gchar *text, gboolean is_kde);
static void copy_via_subprocess (const gchar *text);
static void clear_file_monitors (void);
static void sync_file_monitors (GPtrArray *desired_paths);
static void on_db_file_changed (GFileMonitor *monitor, GFile *file, GFile *other,
                                GFileMonitorEvent event, gpointer user_data);
static void kdf_cache_entry_free (KdfCacheEntry *entry);
static void kdf_cache_invalidate_path (const gchar *db_path);
static void kdf_cache_clear (void);
static void kdf_cache_apply_to_db_data (DatabaseData *db_data, const gchar *db_path);
static void kdf_cache_capture_from_db_data (const DatabaseData *db_data, const gchar *db_path);
static gboolean rate_bucket_consume (const gchar *sender);
static void rate_buckets_clear (void);
static gboolean idle_wipe_check (gpointer user_data);
static gchar *normalize_terms (gchar **terms);
static gchar *issue_activation_capability (const gchar          *sender,
                                           const gchar          *query,
                                           const OtpSearchEntry *entry);
static ActivationCapability *consume_activation_capability (const gchar *id,
                                                            const gchar *sender,
                                                            const gchar *query);
static ActivationCapability *lookup_activation_capability (const gchar *id,
                                                           const gchar *sender);
static void activation_capability_free (ActivationCapability *cap);
static void activation_capabilities_clear (void);

static const gchar *krunner_introspection_xml =
"<node>"
"  <interface name='org.kde.krunner1'>"
"    <method name='Match'><arg type='s' name='query' direction='in'/><arg type='a(sssida{sv})' name='matches' direction='out'/></method>"
"    <method name='Run'><arg type='s' name='id' direction='in'/><arg type='s' name='actionId' direction='in'/></method>"
"    <method name='Actions'><arg type='a(sss)' name='actions' direction='out'/></method>"
"  </interface>"
"</node>";

static const gchar *gnome_introspection_xml =
"<node>"
"  <interface name='org.gnome.Shell.SearchProvider2'>"
"    <method name='GetInitialResultSet'><arg type='as' name='terms' direction='in'/><arg type='as' name='results' direction='out'/></method>"
"    <method name='GetSubsearchResultSet'><arg type='as' name='prev' direction='in'/><arg type='as' name='terms' direction='in'/><arg type='as' name='results' direction='out'/></method>"
"    <method name='GetResultMetas'><arg type='as' name='results' direction='in'/><arg type='aa{sv}' name='metas' direction='out'/></method>"
"    <method name='ActivateResult'><arg type='s' name='id' direction='in'/><arg type='as' name='terms' direction='in'/><arg type='u' name='t' direction='in'/></method>"
"    <method name='LaunchSearch'><arg type='as' name='terms' direction='in'/><arg type='u' name='t' direction='in'/></method>"
"  </interface>"
"</node>";




static void
otp_search_entry_free (OtpSearchEntry *entry)
{
    if (!entry) return;
    g_free (entry->id);
    g_free (entry->label);
    g_free (entry->issuer);
    g_free (entry->db_name);
    g_free (entry->db_path);
    g_free (entry->token_identity);
    g_free (entry->label_fold);
    g_free (entry->issuer_fold);
    g_free (entry);
}


static void
kdf_cache_entry_free (KdfCacheEntry *entry)
{
    if (entry == NULL)
        return;
    if (entry->derived_key != NULL) {
        explicit_bzero (entry->derived_key, ARGON2ID_KEYLEN);
        gcry_free (entry->derived_key);
    }
    explicit_bzero (entry->salt, sizeof (entry->salt));
    explicit_bzero (entry->pwd_hash, sizeof (entry->pwd_hash));
    g_free (entry);
}


static void
kdf_cache_invalidate_path (const gchar *db_path)
{
    if (g_kdf_cache == NULL || db_path == NULL)
        return;
    g_hash_table_remove (g_kdf_cache, db_path);
}


static void
kdf_cache_clear (void)
{
    if (g_kdf_cache == NULL)
        return;
    g_hash_table_destroy (g_kdf_cache);
    g_kdf_cache = NULL;
}


/* Populate db_data's KDF cache fields from g_kdf_cache so load_db's
 * try_decrypt_v2 path hits its salt+pwd_hash lookup and skips Argon2id.
 * No-op on cache miss. */
static void
kdf_cache_apply_to_db_data (DatabaseData *db_data,
                            const gchar  *db_path)
{
    if (g_kdf_cache == NULL || db_data == NULL || db_path == NULL)
        return;
    KdfCacheEntry *cache = g_hash_table_lookup (g_kdf_cache, db_path);
    if (cache == NULL || cache->derived_key == NULL)
        return;
    if (db_data->cached_derived_key == NULL)
        db_data->cached_derived_key = gcry_malloc_secure (ARGON2ID_KEYLEN);
    if (db_data->cached_derived_key == NULL)
        return;
    memcpy (db_data->cached_derived_key, cache->derived_key, ARGON2ID_KEYLEN);
    memcpy (db_data->cached_salt, cache->salt, KDF_SALT_SIZE);
    memcpy (db_data->cached_pwd_hash, cache->pwd_hash, sizeof (cache->pwd_hash));
    db_data->has_cached_key = TRUE;
}


/* Copy db_data's (just-populated by try_decrypt_v2) KDF cache fields into
 * g_kdf_cache so the next call can reuse them. Called only after a successful
 * load_db, when has_cached_key is guaranteed TRUE. */
static void
kdf_cache_capture_from_db_data (const DatabaseData *db_data,
                                const gchar        *db_path)
{
    if (db_data == NULL || db_path == NULL || !db_data->has_cached_key ||
        db_data->cached_derived_key == NULL)
        return;
    if (g_kdf_cache == NULL)
        g_kdf_cache = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             g_free,
                                             (GDestroyNotify) kdf_cache_entry_free);
    KdfCacheEntry *entry = g_new0 (KdfCacheEntry, 1);
    entry->derived_key = gcry_malloc_secure (ARGON2ID_KEYLEN);
    if (entry->derived_key == NULL) {
        g_free (entry);
        return;
    }
    memcpy (entry->derived_key, db_data->cached_derived_key, ARGON2ID_KEYLEN);
    memcpy (entry->salt, db_data->cached_salt, KDF_SALT_SIZE);
    memcpy (entry->pwd_hash, db_data->cached_pwd_hash, sizeof (entry->pwd_hash));
    g_hash_table_replace (g_kdf_cache, g_strdup (db_path), entry);
}


/* Returns TRUE if the call should proceed (a token was available), FALSE if
 * the sender's bucket is empty. Tokens refill at RATE_REFILL_PER_SEC up to
 * RATE_BUCKET_MAX, per sender. A NULL sender (peer-to-peer connection without
 * a name) is bucketed under a fixed key so it can't bypass the limiter by
 * being unidentifiable. */
static void
rate_bucket_refill (RateBucket *bucket,
                    gint64      now)
{
    if (bucket->last_refill_us == 0) {
        bucket->tokens = RATE_BUCKET_MAX;
        bucket->last_refill_us = now;
    } else {
        gdouble elapsed_sec = (gdouble) (now - bucket->last_refill_us) / 1.0e6;
        if (elapsed_sec > 0) {
            bucket->tokens += elapsed_sec * RATE_REFILL_PER_SEC;
            if (bucket->tokens > RATE_BUCKET_MAX)
                bucket->tokens = RATE_BUCKET_MAX;
            bucket->last_refill_us = now;
        }
    }
}

/* Drop senders idle beyond the TTL. Only called when the map is at its cap,
 * so the common case never walks the table. */
static void
rate_buckets_evict_stale (gint64 now)
{
    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init (&iter, g_rate_buckets);
    while (g_hash_table_iter_next (&iter, &key, &value)) {
        RateBucket *bucket = value;
        if (bucket->last_seen_us != 0 &&
            now - bucket->last_seen_us >= RATE_BUCKET_TTL_US)
            g_hash_table_iter_remove (&iter);
    }
}

static gboolean
rate_bucket_consume (const gchar *sender)
{
    gint64 now = g_get_monotonic_time ();

    if (g_rate_buckets == NULL)
        g_rate_buckets = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, g_free);

    const gchar *key = (sender != NULL && sender[0] != '\0') ? sender : ":anon";

    if (g_hash_table_size (g_rate_buckets) >= RATE_BUCKETS_MAX_SENDERS) {
        rate_buckets_evict_stale (now);
        if (g_hash_table_size (g_rate_buckets) >= RATE_BUCKETS_MAX_SENDERS) {
            /* Every slot is held by a recently active sender. Drop the
             * least-recently-seen one: a hostile peer still cannot exceed
             * RATE_BUCKET_MAX per sender, and the real user keeps a slot. */
            GHashTableIter iter;
            gpointer key_iter, value_iter;
            const gchar *lru_key = NULL;
            gint64 lru_seen = G_MAXINT64;
            g_hash_table_iter_init (&iter, g_rate_buckets);
            while (g_hash_table_iter_next (&iter, &key_iter, &value_iter)) {
                RateBucket *bucket = value_iter;
                if (bucket->last_seen_us < lru_seen) {
                    lru_seen = bucket->last_seen_us;
                    lru_key = key_iter;
                }
            }
            if (lru_key != NULL)
                g_hash_table_remove (g_rate_buckets, lru_key);
        }
    }

    RateBucket *bucket = g_hash_table_lookup (g_rate_buckets, key);
    if (bucket == NULL) {
        bucket = g_new0 (RateBucket, 1);
        g_hash_table_insert (g_rate_buckets, g_strdup (key), bucket);
    }
    rate_bucket_refill (bucket, now);
    bucket->last_seen_us = now;

    if (bucket->tokens < 1.0)
        return FALSE;
    bucket->tokens -= 1.0;
    return TRUE;
}


static void
rate_buckets_clear (void)
{
    g_clear_pointer (&g_rate_buckets, g_hash_table_destroy);
}

static gboolean
idle_wipe_check (gpointer user_data)
{
    (void) user_data;
    gint64 now = g_get_monotonic_time ();
    if (g_last_activity_us != 0 &&
        now - g_last_activity_us >= IDLE_WIPE_SECONDS * G_USEC_PER_SEC) {
        kdf_cache_clear ();
        g_clear_pointer (&cached_entries, g_ptr_array_unref);
        cached_at = 0;
        activation_capabilities_clear ();
        rate_buckets_clear ();
        g_last_activity_us = 0;
    }
    return G_SOURCE_CONTINUE;
}


static void
activation_capability_free (ActivationCapability *cap)
{
    if (cap == NULL)
        return;
    g_free (cap->id);
    g_free (cap->sender);
    g_free (cap->query);
    g_free (cap->db_path);
    g_free (cap->label);
    g_free (cap->token_identity);
    g_free (cap);
}


static void
activation_capabilities_clear (void)
{
    if (g_activation_caps == NULL)
        return;
    g_hash_table_destroy (g_activation_caps);
    g_activation_caps = NULL;
}


static void
activation_capabilities_prune (void)
{
    if (g_activation_caps == NULL)
        return;
    gint64 now = g_get_monotonic_time ();
    GHashTableIter iter;
    gpointer value;
    g_hash_table_iter_init (&iter, g_activation_caps);
    while (g_hash_table_iter_next (&iter, NULL, &value)) {
        ActivationCapability *cap = value;
        if (cap->expires_at_us <= now)
            g_hash_table_iter_remove (&iter);
    }
}


static gchar *
normalize_terms (gchar **terms)
{
    if (terms == NULL)
        return g_strdup ("");
    GString *joined = g_string_new (NULL);
    for (gsize i = 0; terms[i] != NULL; i++) {
        if (terms[i][0] == '\0')
            continue;
        g_autofree gchar *fold = g_utf8_casefold (terms[i], -1);
        if (joined->len > 0)
            g_string_append_c (joined, '\x1f');
        g_string_append (joined, fold);
    }
    return g_string_free (joined, FALSE);
}


static gchar *
issue_activation_capability (const gchar          *sender,
                             const gchar          *query,
                             const OtpSearchEntry *entry)
{
    if (entry == NULL)
        return NULL;
    if (g_activation_caps == NULL)
        g_activation_caps = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                   g_free,
                                                   (GDestroyNotify) activation_capability_free);
    activation_capabilities_prune ();

    guint8 raw[16];
    gcry_create_nonce (raw, sizeof (raw));
    gchar *id = bytes_to_hexstr (raw, sizeof (raw));
    explicit_bzero (raw, sizeof (raw));
    if (id == NULL)
        return NULL;

    ActivationCapability *cap = g_new0 (ActivationCapability, 1);
    cap->id = g_strdup (id);
    cap->sender = g_strdup ((sender != NULL && sender[0] != '\0') ? sender : ":anon");
    cap->query = g_strdup (query != NULL ? query : "");
    cap->db_path = g_strdup (entry->db_path);
    cap->label = g_strdup (entry->label);
    cap->token_identity = g_strdup (entry->token_identity);
    cap->json_index = entry->json_index;
    cap->expires_at_us = g_get_monotonic_time () + ACTIVATION_CAP_TTL_US;
    g_hash_table_insert (g_activation_caps, g_strdup (id), cap);
    return id;
}


static ActivationCapability *
lookup_activation_capability (const gchar *id,
                              const gchar *sender)
{
    if (g_activation_caps == NULL || id == NULL)
        return NULL;
    activation_capabilities_prune ();
    ActivationCapability *cap = g_hash_table_lookup (g_activation_caps, id);
    const gchar *sender_key = (sender != NULL && sender[0] != '\0') ? sender : ":anon";
    if (cap == NULL || g_strcmp0 (cap->sender, sender_key) != 0)
        return NULL;
    return cap;
}


static ActivationCapability *
consume_activation_capability (const gchar *id,
                               const gchar *sender,
                               const gchar *query)
{
    ActivationCapability *cap = lookup_activation_capability (id, sender);
    if (cap == NULL)
        return NULL;
    if (query != NULL && g_strcmp0 (cap->query, query) != 0)
        return NULL;

    gpointer key = NULL;
    gpointer value = NULL;
    if (!g_hash_table_steal_extended (g_activation_caps, id, &key, &value))
        return NULL;
    g_free (key);
    return value;
}


static gchar *
get_entry_otp_value (json_t *obj)
{
    GError *validation_err = NULL;
    if (!otp_validate_token_object (obj, 0, &validation_err)) {
        g_warning ("Search provider: refusing invalid OTP token: %s",
                   validation_err != NULL ? validation_err->message : "unknown validation error");
        g_clear_error (&validation_err);
        return NULL;
    }

    const gchar *secret = json_string_value (json_object_get (obj, "secret"));
    const gchar *type = json_string_value (json_object_get (obj, "type"));
    if (!secret || !type) return NULL;

    cotp_error_t cotp_err;
    const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
    gint digits = (gint)json_integer_value (json_object_get (obj, "digits"));
    gint algo = get_algo_int_from_str (json_string_value (json_object_get (obj, "algo")));
    gchar *token = NULL;

    if (g_ascii_strcasecmp (type, "TOTP") == 0) {
        gint period = (gint)json_integer_value (json_object_get (obj, "period"));
        time_t now = time (NULL);
        if (now < 0 || (guint64) now > (guint64) LONG_MAX)
            return NULL;
        long current_ts = (long) now;
        if (issuer != NULL && g_ascii_strcasecmp (issuer, "steam") == 0) {
            token = get_steam_totp_at (secret, current_ts, period, &cotp_err);
        } else {
            token = get_totp_at (secret, current_ts, digits, period, algo, &cotp_err);
        }
    }

    if (token == NULL) return NULL;
    gchar *result = secure_strdup (token);
    sensitive_free (token);
    return result;
}


/* A stable identity for a token: issuer, label and the 32-bit content hash of
 * the whole token (which includes the secret). issuer+label alone is not
 * unique - a database can hold two different tokens with the same name - and
 * matching on it alone would resolve a result to the wrong secret. The hash
 * is not reversible and is never persisted; it only lives in the capability
 * and activation job for the 30 s TTL. Ambiguous matches are rejected by the
 * caller rather than guessed. */
static gchar *
token_identity_from_obj (json_t *obj)
{
    const gchar *label = json_string_value (json_object_get (obj, "label"));
    if (label == NULL)
        return NULL;
    const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
    guint32 hash = json_object_get_hash (obj);
    return g_strdup_printf ("%s\x1f%s\x1f%08x",
                            issuer != NULL ? issuer : "", label, hash);
}


/* TRUE when @obj is the token named by @identity. */
static gboolean
token_obj_matches_identity (json_t     *obj,
                            const gchar *identity)
{
    if (identity == NULL)
        return FALSE;
    g_autofree gchar *cand = token_identity_from_obj (obj);
    return cand != NULL && g_strcmp0 (cand, identity) == 0;
}


/* Finds the token named by @identity in @root. @preferred_index (the array
 * position captured at query time) wins when it still names that token, so two
 * distinct tokens sharing a name resolve to the one actually selected. Only
 * when the index no longer matches do we scan; an ambiguous scan (more than one
 * match, e.g. a 32-bit hash collision) is rejected rather than guessed.
 * Returns NULL and sets *out_failure when missing or ambiguous. */
static json_t *
find_token_by_identity (json_t      *root,
                        const gchar *identity,
                        gsize        preferred_index,
                        gchar      **out_failure)
{
    json_t *obj = NULL;
    if (identity != NULL && preferred_index < json_array_size (root)) {
        json_t *at_index = json_array_get (root, preferred_index);
        if (token_obj_matches_identity (at_index, identity))
            obj = at_index;
    }

    if (obj == NULL) {
        guint matches = 0;
        gsize idx;
        json_t *candidate;
        json_array_foreach (root, idx, candidate) {
            if (token_obj_matches_identity (candidate, identity)) {
                obj = candidate;
                matches++;
            }
        }
        if (matches > 1) {
            *out_failure = g_strdup ("More than one account matches that result. Open OTPClient and try again.");
            return NULL;
        }
    }

    if (obj == NULL)
        *out_failure = g_strdup ("That account is no longer in the database.");
    return obj;
}


/* The keyring lookup happens in entries_reload_step, asynchronously; by the time
 * we get here the password is in hand and everything left is local work. */
static void
load_entries_from_db (GPtrArray   *entries,
                      const gchar *db_path,
                      const gchar *db_name,
                      guint        db_index,
                      const gchar *password)
{
    DatabaseData *db_data = database_data_new (db_path, global_max_file_size);
    db_data->key = secure_strdup (password);

    /* Same cache reuse as compute_otp_for_entry: skip Argon2id on every
     * subsequent entries reload (prewarm, TTL expiry) if the derived key
     * for this db_path is still in g_kdf_cache. */
    kdf_cache_apply_to_db_data (db_data, db_path);

    GError *err = NULL;
    load_db (db_data, &err);
    if (err != NULL || db_data->in_memory_json_data == NULL)
    {
        if (err != NULL) g_clear_error (&err);
        database_data_free (db_data);
        return;
    }

    /* Issue #464: broken tokens are set aside so search still works with the
     * rest; note it in the journal without spamming (this reloads on a TTL). */
    guint quarantined = db_get_quarantined_count (db_data);
    if (quarantined > 0)
        g_info ("%u token(s) in '%s' could not be loaded and were skipped.", quarantined, db_path);

    kdf_cache_capture_from_db_data (db_data, db_path);

    gsize index;
    json_t *obj;
    json_array_foreach (db_data->in_memory_json_data, index, obj)
    {
        const gchar *label = json_string_value (json_object_get (obj, "label"));
        if (label == NULL) continue;
        const gchar *type = json_string_value (json_object_get (obj, "type"));
        if (type == NULL || g_ascii_strcasecmp (type, "TOTP") != 0)
            continue;
        const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
        OtpSearchEntry *entry = g_new0 (OtpSearchEntry, 1);
        entry->id = g_strdup_printf ("%u:%" G_GSIZE_FORMAT, db_index, index);
        entry->label = g_strdup (label);
        entry->issuer = g_strdup (issuer ? issuer : "");
        entry->db_name = g_strdup (db_name);
        entry->db_path = g_strdup (db_path);
        entry->token_identity = token_identity_from_obj (obj);
        entry->json_index = index;
        /* Pre-casefold for entry_matches_terms - done once at load instead of
         * once per term per query. Live OTP codes are no longer cached: they're
         * recomputed on demand in compute_otp_for_entry, so a heap inspection
         * of the daemon shows only labels/issuers, not active codes. */
        entry->label_fold = g_utf8_casefold (entry->label, -1);
        entry->issuer_fold = g_utf8_casefold (entry->issuer, -1);
        g_ptr_array_add (entries, entry);
    }

    database_data_free (db_data);
}

static void
on_db_file_changed (GFileMonitor      *monitor G_GNUC_UNUSED,
                    GFile             *file,
                    GFile             *other G_GNUC_UNUSED,
                    GFileMonitorEvent  event,
                    gpointer           user_data G_GNUC_UNUSED)
{
    /* Invalidate on any event that could change the DB contents. CHANGED
     * fires often during a write; CHANGES_DONE_HINT marks the end of a
     * write batch. Either way, drop the cache so the next query rebuilds. */
    if (event == G_FILE_MONITOR_EVENT_CHANGED ||
        event == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT ||
        event == G_FILE_MONITOR_EVENT_CREATED ||
        event == G_FILE_MONITOR_EVENT_DELETED ||
        event == G_FILE_MONITOR_EVENT_RENAMED ||
        event == G_FILE_MONITOR_EVENT_MOVED_IN ||
        event == G_FILE_MONITOR_EVENT_MOVED_OUT)
    {
        cached_at = 0;
        /* Drop the KDF cache entry for this specific path: a password change
         * yields a new salt + new derived key, so the stale entry would just
         * cause a wasted memcmp on the next call. Wiping it sooner also
         * shortens the lifetime of the old derived key in secure memory. */
        if (file != NULL) {
            g_autofree gchar *path = g_file_get_path (file);
            if (path != NULL)
                kdf_cache_invalidate_path (path);
        }
        /* Pending activation capabilities name tokens resolved against the
         * previous contents. Drop them; an already-started activation job
         * re-resolves its token by identity against the fresh database, so it
         * cannot deliver another token's OTP. */
        activation_capabilities_clear();
    }
}


static void
clear_file_monitors (void)
{
    if (file_monitors == NULL) return;
    g_hash_table_destroy (file_monitors);
    file_monitors = NULL;
}


static void
sync_file_monitors (GPtrArray *desired_paths)
{
    if (file_monitors == NULL)
        file_monitors = g_hash_table_new_full (g_str_hash, g_str_equal,
                                               g_free, g_object_unref);

    /* Drop monitors whose path is no longer in the desired set. */
    GHashTableIter iter;
    gpointer key;
    g_hash_table_iter_init (&iter, file_monitors);
    while (g_hash_table_iter_next (&iter, &key, NULL))
    {
        gboolean wanted = FALSE;
        for (guint i = 0; i < desired_paths->len; i++)
        {
            if (g_strcmp0 (key, g_ptr_array_index (desired_paths, i)) == 0)
            {
                wanted = TRUE;
                break;
            }
        }
        if (!wanted)
            g_hash_table_iter_remove (&iter);
    }

    /* Add monitors for paths we don't already track. */
    for (guint i = 0; i < desired_paths->len; i++)
    {
        const gchar *path = g_ptr_array_index (desired_paths, i);
        if (path == NULL || g_hash_table_contains (file_monitors, path))
            continue;
        g_autoptr(GFile) f = g_file_new_for_path (path);
        g_autoptr(GError) err = NULL;
        GFileMonitor *m = g_file_monitor_file (f, G_FILE_MONITOR_NONE, NULL, &err);
        if (m == NULL)
        {
            if (err != NULL)
                g_warning ("Failed to monitor %s: %s", path, err->message);
            continue;
        }
        g_signal_connect (m, "changed", G_CALLBACK (on_db_file_changed), NULL);
        g_hash_table_insert (file_monitors, g_strdup (path), m);
    }
}


/* One reload walks the configured databases one at a time, waiting on an async
 * keyring lookup for each. At most one runs at a time: queries that arrive while
 * it is in flight are parked on entries_waiters and all get the same answer,
 * instead of each starting its own round-trip against the Secret Service. */
typedef struct {
    GPtrArray *entries;         /* accumulating result, owned */
    GPtrArray *db_list;         /* owned, may be NULL */
    gchar     *fallback_path;   /* owned, may be NULL */
    guint      total;
    guint      next_index;
    /* Borrowed from db_list / fallback_path for the lookup currently in flight. */
    const gchar *cur_path;
    const gchar *cur_name;
    /* Settings generation the reload started under. See entries_reload_complete. */
    guint64    generation;
    /* Bounds the in-flight keyring lookup: if the Secret Service wedges, the
     * lookup never completes and this reload (plus every queued query) stalls
     * forever. The deadline cancels the lookup; the completion callback then
     * observes the timeout and moves on to the next database. */
    GCancellable *keyring_cancellable;
    guint         keyring_deadline_id;
    gboolean      keyring_timed_out;
} EntriesReload;

static EntriesReload *entries_reload = NULL;
static GSList        *entries_waiters = NULL;   /* GTask *, owned */

static void entries_reload_step (EntriesReload *reload);

/* A wedged Secret Service must not leave an ActivationJob or a reload parked
 * forever: bound every keyring lookup with a cancellable deadline. */
#define KEYRING_LOOKUP_DEADLINE_SECONDS 30


static gboolean
ensure_crypto_ready (void)
{
    if (crypto_ready)
        return TRUE;

    if (set_memlock_value (&global_max_file_size) == MEMLOCK_ERR) {
        g_warning ("Search provider: couldn't get the memlock value, databases cannot be opened.");
        return FALSE;
    }
    gchar *init_msg = init_libs (global_max_file_size);
    if (init_msg != NULL) {
        g_warning ("Search provider: error while initializing GCrypt: %s", init_msg);
        g_free (init_msg);
        return FALSE;
    }
    crypto_ready = TRUE;
    return TRUE;
}


static void
entries_reload_free (EntriesReload *reload)
{
    if (reload == NULL)
        return;
    if (reload->keyring_deadline_id != 0) {
        g_source_remove (reload->keyring_deadline_id);
        reload->keyring_deadline_id = 0;
    }
    g_clear_object (&reload->keyring_cancellable);
    g_clear_pointer (&reload->entries, g_ptr_array_unref);
    g_clear_pointer (&reload->db_list, g_ptr_array_unref);
    g_clear_pointer (&reload->fallback_path, g_free);
    g_free (reload);
}

static gboolean on_reload_keyring_deadline (gpointer user_data);

static void
entries_reload_arm_keyring_guard (EntriesReload *reload)
{
    if (reload->keyring_deadline_id != 0) {
        g_source_remove (reload->keyring_deadline_id);
        reload->keyring_deadline_id = 0;
    }
    g_clear_object (&reload->keyring_cancellable);
    reload->keyring_timed_out = FALSE;
    reload->keyring_cancellable = g_cancellable_new ();
    reload->keyring_deadline_id = g_timeout_add_seconds (
        KEYRING_LOOKUP_DEADLINE_SECONDS, on_reload_keyring_deadline, reload);
}

static gboolean
on_reload_keyring_deadline (gpointer user_data)
{
    EntriesReload *reload = user_data;
    reload->keyring_deadline_id = 0;
    reload->keyring_timed_out = TRUE;
    g_warning ("Search provider: keyring lookup for %s did not answer within %d s; skipping it.",
               reload->cur_path != NULL ? reload->cur_path : "(unknown)",
               KEYRING_LOOKUP_DEADLINE_SECONDS);
    g_cancellable_cancel (reload->keyring_cancellable);
    return G_SOURCE_REMOVE;
}


static void
entries_reload_complete (EntriesReload *reload)
{
    entries_reload = NULL;
    GSList *waiters = entries_waiters;
    entries_waiters = NULL;

    GPtrArray *result = NULL;
    if (reload->generation != delivery_generation) {
        /* The keyword, the enabled switch or the secret-service setting moved
         * while we were waiting on the keyring, so this answer describes a
         * configuration that no longer applies. Drop it rather than caching it;
         * the next query rebuilds. */
        result = g_ptr_array_new_with_free_func ((GDestroyNotify) otp_search_entry_free);
    } else {
        g_clear_pointer (&cached_entries, g_ptr_array_unref);
        cached_entries = g_steal_pointer (&reload->entries);
        cached_at = time (NULL);
        result = g_ptr_array_ref (cached_entries);
    }
    entries_reload_free (reload);

    for (GSList *l = waiters; l != NULL; l = l->next) {
        GTask *task = l->data;
        g_task_return_pointer (task, g_ptr_array_ref (result), (GDestroyNotify) g_ptr_array_unref);
        g_object_unref (task);
    }
    g_slist_free (waiters);
    g_ptr_array_unref (result);
}


static void
on_reload_password (GObject      *source G_GNUC_UNUSED,
                    GAsyncResult *res,
                    gpointer      user_data)
{
    EntriesReload *reload = user_data;
    if (reload->keyring_deadline_id != 0) {
        g_source_remove (reload->keyring_deadline_id);
        reload->keyring_deadline_id = 0;
    }
    g_clear_object (&reload->keyring_cancellable);
    g_autoptr (GError) err = NULL;
    gchar *pwd = otpclient_secret_lookup_with_legacy_fallback_finish (res, NULL, &err);

    if (reload->keyring_timed_out) {
        /* The deadline already fired and cancelled this lookup; the next
         * database still gets its turn instead of the whole reload (and
         * every queued query with it) stalling forever. */
        reload->keyring_timed_out = FALSE;
    } else if (err != NULL) {
        /* Issue #446: surface broken-keyring errors via a warning instead of
         * silently returning. Don't mutate GSettings here, the search provider
         * is a passive consumer; the GUI app owns the setting. */
        g_warning ("Search provider: secret service lookup failed for %s: %s",
                   reload->cur_path, err->message);
    } else if (pwd != NULL) {
        load_entries_from_db (reload->entries, reload->cur_path, reload->cur_name,
                              reload->next_index, pwd);
    }
    /* Cancellation can race a lookup that has already completed and queued
     * this callback. In that case keyring_timed_out is TRUE but _finish still
     * returns the password, so it must be wiped regardless of which branch
     * above handled the result. */
    if (pwd != NULL)
        secret_password_free (pwd);

    reload->next_index++;
    entries_reload_step (reload);
}


static void
entries_reload_step (EntriesReload *reload)
{
    if (reload->next_index >= reload->total) {
        entries_reload_complete (reload);
        return;
    }

    if (reload->db_list != NULL && reload->db_list->len > 0) {
        DbListEntry *dbe = g_ptr_array_index (reload->db_list, reload->next_index);
        reload->cur_path = dbe->path;
        reload->cur_name = dbe->name;
    } else {
        reload->cur_path = reload->fallback_path;
        reload->cur_name = NULL;
    }

    if (reload->cur_path == NULL) {
        reload->next_index++;
        entries_reload_step (reload);
        return;
    }

    /* Issue #448: try the v4 "main_pwd" entry too when the db_path-keyed lookup
     * misses, so users who upgraded but have not opened the GUI yet still get
     * search hits. No cleanup here, the GUI's first launch is what migrates and
     * clears the legacy entry. */
    entries_reload_arm_keyring_guard (reload);
    otpclient_secret_lookup_with_legacy_fallback_async (reload->cur_path,
                                                        reload->keyring_cancellable,
                                                        on_reload_password, reload);
}


static void
entries_reload_start (void)
{
    EntriesReload *reload = g_new0 (EntriesReload, 1);
    reload->entries = g_ptr_array_new_with_free_func ((GDestroyNotify) otp_search_entry_free);
    reload->generation = delivery_generation;

    /* Collect the desired set of paths, then diff our current monitor set
     * against it - see sync_file_monitors. */
    g_autoptr (GPtrArray) desired_paths = g_ptr_array_new ();

    reload->db_list = gsettings_common_get_db_list ();
    if (reload->db_list != NULL && reload->db_list->len > 0) {
        for (guint i = 0; i < reload->db_list->len; i++) {
            DbListEntry *dbe = g_ptr_array_index (reload->db_list, i);
            if (dbe->path != NULL)
                g_ptr_array_add (desired_paths, dbe->path);
        }
        reload->total = reload->db_list->len;
    } else {
        reload->fallback_path = gsettings_common_get_db_path ();
        if (reload->fallback_path != NULL) {
            g_ptr_array_add (desired_paths, reload->fallback_path);
            reload->total = 1;
        }
    }

    sync_file_monitors (desired_paths);

    /* Both of these would make every per-database step a no-op, so skip straight
     * to the (empty) result rather than walking the list. */
    if (!gsettings_common_get_use_secret_service () || !ensure_crypto_ready ())
        reload->total = 0;

    entries_reload = reload;
    entries_reload_step (reload);
}


/* Returns the cached entry list, reloading it from disk and the keyring first if
 * the cache has gone stale. The result is a ref on the shared array: hold it for
 * as long as you iterate, since a concurrent reload can replace the cache. */
static void
get_entries_async (GAsyncReadyCallback callback,
                   gpointer            user_data)
{
    GTask *task = g_task_new (NULL, NULL, callback, user_data);
    g_task_set_source_tag (task, get_entries_async);

    gint64 now = time (NULL);
    if (cached_entries != NULL && cached_at != 0 && (now - cached_at) < CACHE_TTL_SECONDS) {
        g_task_return_pointer (task, g_ptr_array_ref (cached_entries),
                               (GDestroyNotify) g_ptr_array_unref);
        g_object_unref (task);
        return;
    }

    entries_waiters = g_slist_append (entries_waiters, task);
    if (entries_reload == NULL)
        entries_reload_start ();
}


static GPtrArray *
get_entries_finish (GAsyncResult *result)
{
    return g_task_propagate_pointer (G_TASK (result), NULL);
}


static void
load_keyword_config (void)
{
    g_free (g_keyword);
    g_free (g_keyword_fold);
    g_keyword = gsettings_common_get_search_provider_keyword ();
    if (g_keyword == NULL) g_keyword = g_strdup ("");
    g_strstrip (g_keyword);
    /* Defense in depth against arbitrary dconf-edits: a runaway-length keyword
     * would still get casefolded and compared on every query. Truncate by
     * UTF-8 character count so we don't slice a multi-byte sequence. */
    glong char_len = g_utf8_strlen (g_keyword, -1);
    if (char_len > OTPCLIENT_SEARCH_KEYWORD_MAX_LEN) {
        const gchar *cut = g_utf8_offset_to_pointer (g_keyword, OTPCLIENT_SEARCH_KEYWORD_MAX_LEN);
        *((gchar *) cut) = '\0';
    }
    g_keyword_fold = g_utf8_casefold (g_keyword, -1);
}


static gboolean
provider_access_allowed (void)
{
    return gsettings_common_get_search_provider_enabled () &&
           gsettings_common_get_use_secret_service () &&
           g_keyword_fold != NULL && g_keyword_fold[0] != '\0';
}

static void
on_provider_settings_changed (GSettings *settings, const gchar *key, gpointer data)
{
    (void) settings;
    (void) data;
    if (!g_str_equal (key, "search-provider-enabled") &&
        !g_str_equal (key, "search-provider-keyword") &&
        !g_str_equal (key, "secret-service"))
        return;
    delivery_generation++;
    load_keyword_config ();
    g_clear_pointer (&cached_entries, g_ptr_array_unref);
    cached_at = 0;
    kdf_cache_clear ();
    activation_capabilities_clear ();
    clear_file_monitors ();
}

static void
return_disabled_result (const gchar *method, GDBusMethodInvocation *inv)
{
    const gchar *type = NULL;
    if (g_str_equal (method, "GetInitialResultSet") || g_str_equal (method, "GetSubsearchResultSet"))
        type = "as";
    else if (g_str_equal (method, "GetResultMetas"))
        type = "aa{sv}";
    else if (g_str_equal (method, "Match"))
        type = "a(sssida{sv})";
    else if (g_str_equal (method, "Actions"))
        type = "a(sss)";
    if (type != NULL) {
        GVariant *empty = g_variant_new_array (g_variant_type_element (G_VARIANT_TYPE (type)), NULL, 0);
        g_dbus_method_invocation_return_value (inv, g_variant_new_tuple (&empty, 1));
    } else {
        g_dbus_method_invocation_return_value (inv, NULL);
    }
}

/* Returns TRUE and writes the post-keyword tail into *out_terms (caller frees
 * with g_strfreev) when the first non-empty term equals the configured
 * keyword AND there is at least one further non-empty term. Returns FALSE and
 * leaves *out_terms NULL otherwise.
 *
 * Threat model: we share the session bus with browser extensions, Electron
 * apps, and arbitrary user-installed software. Without a keyword filter, any
 * such process can poll for OTP entries and (via Run/ActivateResult) trigger
 * a notification carrying the live code. Treat an empty/unset keyword as
 * "search provider disabled" rather than "no filter": users opting in to the
 * provider have to set a keyword explicitly. The notification surfacing the
 * code is still user-visible; the keyword is the gate, not a complete defense. */
static gboolean
strip_keyword_or_skip (gchar  **terms,
                       gchar ***out_terms)
{
    *out_terms = NULL;
    if (terms == NULL) return FALSE;

    /* H3: refuse all queries when keyword is empty. Previously this branch
     * fell through to substring matching, exposing every account to any local
     * D-Bus client by default. */
    if (g_keyword_fold == NULL || g_keyword_fold[0] == '\0') {
        return FALSE;
    }

    gsize i = 0;
    while (terms[i] != NULL && terms[i][0] == '\0') i++;
    if (terms[i] == NULL) return FALSE;

    g_autofree gchar *first_fold = g_utf8_casefold (terms[i], -1);
    if (g_strcmp0 (first_fold, g_keyword_fold) != 0) return FALSE;

    GPtrArray *tail = g_ptr_array_new ();
    for (gsize j = i + 1; terms[j] != NULL; j++) {
        if (terms[j][0] != '\0') g_ptr_array_add (tail, g_strdup (terms[j]));
    }
    if (tail->len == 0) {
        g_ptr_array_free (tail, TRUE);
        return FALSE;
    }
    g_ptr_array_add (tail, NULL);
    *out_terms = (gchar **) g_ptr_array_free (tail, FALSE);
    return TRUE;
}


static gboolean
entry_matches_terms (const OtpSearchEntry *entry,
                     gchar               **terms,
                     gsize                 terms_len)
{
    if (terms_len == 0 || !entry->label_fold) return FALSE;
    for (gsize i = 0; i < terms_len; i++) {
        if (!terms[i]) continue;
        g_autofree gchar *t_fold = g_utf8_casefold (terms[i], -1);
        if (!g_strstr_len (entry->label_fold, -1, t_fold) &&
            !g_strstr_len (entry->issuer_fold, -1, t_fold))
            return FALSE;
    }
    return TRUE;
}


/* Open, decrypt, look up the JSON object, compute the OTP, then wipe everything.
 * The Argon2id derivation pays the user-visible latency, but the cached derived
 * key in db_data->cached_derived_key + the file monitor + the 60 s entry cache
 * mean this only happens on the first Run after the DB has changed. The trade vs
 * caching the OTP value: heap inspection of the daemon never reveals an active
 * OTP.
 *
 * The password comes from the caller, which does the keyring lookup
 * asynchronously. On failure, *out_failure gets a short sentence fit to put in
 * front of the user: activation used to fail completely silently, with no toast,
 * no error and not even a journal line, so a result that could not be turned
 * into a code looked exactly like a result that had been ignored. */
static gchar *
compute_otp_with_password (const gchar  *db_path,
                           const gchar  *token_identity,
                           gsize         json_index,
                           const gchar  *password,
                           gchar       **out_failure)
{
    if (db_path == NULL || password == NULL) return NULL;
    if (!provider_access_allowed ()) return NULL;
    if (!ensure_crypto_ready ()) {
        *out_failure = g_strdup ("Secure memory is unavailable, so the database cannot be opened.");
        return NULL;
    }

    DatabaseData *db_data = database_data_new (db_path, global_max_file_size);
    db_data->key = secure_strdup (password);

    /* Pre-load the cached Argon2id derived key (if any) before load_db so
     * try_decrypt_v2's salt+pwd_hash lookup hits and skips the 150-300 ms
     * derivation. A mismatch (changed password) falls through to a fresh
     * derive and overwrites the cache below. */
    kdf_cache_apply_to_db_data (db_data, db_path);

    GError *err = NULL;
    load_db (db_data, &err);
    gchar *otp = NULL;
    if (err == NULL && db_data->in_memory_json_data != NULL) {
        /* Re-find the token by its stable identity instead of blindly trusting
         * the index captured at query time: the array may have been reordered
         * or had entries inserted/removed by a write in the meantime. */
        json_t *obj = find_token_by_identity (db_data->in_memory_json_data,
                                              token_identity, json_index,
                                              out_failure);
        if (obj != NULL && (otp = get_entry_otp_value (obj)) == NULL)
            *out_failure = g_strdup ("The code for that account could not be generated.");
        /* try_decrypt_v2 populates db_data->cached_* on success; persist
         * those into g_kdf_cache so the next call hits. Capturing only on
         * success keeps a wrong-password attempt from poisoning the cache. */
        kdf_cache_capture_from_db_data (db_data, db_path);
    } else {
        /* Most often the stored password no longer matches the database. The
         * underlying message names the file and the reason, which is what the
         * journal wants; the notification stays short. */
        g_warning ("Search provider: could not open '%s': %s", db_path,
                   err != NULL ? err->message : "the database is empty or unreadable");
        *out_failure = g_strdup ("The database could not be opened. Is the stored password still correct?");
    }
    if (err != NULL) g_clear_error (&err);

    database_data_free (db_data);

    return otp;
}


typedef struct {
    gchar *text;
    guint64 generation;
} ClipboardDelivery;

static void
klipper_copy_done (GObject      *source,
                   GAsyncResult *res,
                   gpointer      user_data)
{
    ClipboardDelivery *delivery = user_data;
    gchar *text = delivery->text;
    g_autoptr (GError) error = NULL;
    g_autoptr (GVariant) reply =
        g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, &error);
    /* A timeout means Klipper received setClipboardContents (and therefore set
     * the clipboard) but did not reply in time, so do NOT also copy via the CLI
     * tools. Any other error means Klipper is genuinely unreachable: fall back
     * so users without Klipper still get the code. */
    if (delivery->generation == delivery_generation && provider_access_allowed () &&
        reply == NULL && text != NULL &&
        !g_error_matches (error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
        copy_via_subprocess (text);
    if (text != NULL)
        sensitive_secure_free (text);
    g_free (delivery);
}


static void
copy_via_klipper (GDBusConnection *conn,
                  const gchar     *text)
{
    /* Fire-and-forget: dispatch the clipboard write to Klipper without blocking
     * the activation handler on its reply. Klipper sets the clipboard as soon
     * as it processes the message; the previous synchronous call waited up to
     * 1000 ms for a reply that may never arrive, stalling every KDE activation
     * by ~1 s. The CLI fallback now happens in klipper_copy_done only when
     * Klipper is actually absent. */
    if (conn == NULL) {
        copy_via_subprocess (text);
        return;
    }
    ClipboardDelivery *delivery = g_new0 (ClipboardDelivery, 1);
    delivery->text = secure_strdup (text);
    delivery->generation = delivery_generation;
    g_dbus_connection_call (conn,
            "org.kde.klipper", "/klipper", "org.kde.klipper.klipper",
            "setClipboardContents", g_variant_new (SP_SIG_KLIPPER_SET_CLIPBOARD, text),
            NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL,
            klipper_copy_done, delivery);
}


/* XDG_SESSION_TYPE is not a reliable answer inside the sandbox: it is whatever
 * the host session exported, and with --socket=fallback-x11 on a Wayland host
 * there is no X socket in the sandbox at all, so guessing X11 there means the
 * copy silently does nothing. WAYLAND_DISPLAY is set by flatpak itself whenever
 * --socket=wayland is in effect, so trust that first and keep XDG_SESSION_TYPE
 * as the fallback for native installs that do not export it. */
static gboolean
session_is_wayland (void)
{
    const gchar *wl_display = g_getenv ("WAYLAND_DISPLAY");
    if (wl_display != NULL && wl_display[0] != '\0')
        return TRUE;

    const gchar *session = g_getenv ("XDG_SESSION_TYPE");
    return session != NULL && g_ascii_strcasecmp (session, "wayland") == 0;
}


/* A clipboard tool normally detaches in milliseconds. The one that does not is
 * xclip or xsel against a wedged X server, or a DISPLAY pointing at a host that
 * never answers: those block on the X connection and never exit. Since this runs
 * from the D-Bus handler, that used to pin the provider's only thread for good.
 * Give each tool a deadline and move on to the next one. */
#define CLIPBOARD_TOOL_TIMEOUT_SECONDS 3

typedef struct {
    gchar        *text;             /* secure copy, wiped on free */
    const gchar **candidates[3];
    guint         next_candidate;
    GSubprocess  *proc;             /* the one currently running */
    guint         timeout_id;
    guint64       generation;
} ClipboardSpawn;

static void clipboard_spawn_step (ClipboardSpawn *spawn);


static void
clipboard_spawn_free (ClipboardSpawn *spawn)
{
    if (spawn == NULL)
        return;
    g_clear_handle_id (&spawn->timeout_id, g_source_remove);
    g_clear_object (&spawn->proc);
    if (spawn->text != NULL)
        sensitive_secure_free (spawn->text);
    g_free (spawn);
}


static gboolean
on_clipboard_tool_timeout (gpointer user_data)
{
    ClipboardSpawn *spawn = user_data;

    spawn->timeout_id = 0;
    g_warning ("Clipboard tool '%s' did not finish within %d seconds; giving up on it",
               spawn->candidates[spawn->next_candidate - 1][0], CLIPBOARD_TOOL_TIMEOUT_SECONDS);
    /* Kills the child, which completes the pending communicate with an error and
     * lands us in on_clipboard_tool_done with the next candidate queued up. */
    if (spawn->proc != NULL)
        g_subprocess_force_exit (spawn->proc);
    return G_SOURCE_REMOVE;
}


static void
on_clipboard_tool_done (GObject      *source,
                        GAsyncResult *res,
                        gpointer      user_data)
{
    ClipboardSpawn *spawn = user_data;
    GSubprocess *proc = G_SUBPROCESS (source);
    g_autoptr (GError) error = NULL;

    g_clear_handle_id (&spawn->timeout_id, g_source_remove);
    /* The exit status matters as much as the spawn: xsel with no X display
     * starts fine and then fails, and treating that as success would stop us
     * trying wl-copy. */
    gboolean ok = g_subprocess_communicate_finish (proc, res, NULL, NULL, &error) &&
                  g_subprocess_get_successful (proc);
    g_clear_object (&spawn->proc);

    if (ok) {
        clipboard_spawn_free (spawn);
        return;
    }
    clipboard_spawn_step (spawn);
}


static void
clipboard_spawn_step (ClipboardSpawn *spawn)
{
    /* A settings change since the copy was requested means the user turned the
     * provider (or the keyword) off mid-flight. Do not keep trying to put a code
     * on their clipboard. */
    if (spawn->generation != delivery_generation || !provider_access_allowed ()) {
        clipboard_spawn_free (spawn);
        return;
    }

    while (spawn->next_candidate < G_N_ELEMENTS (spawn->candidates)) {
        const gchar **argv = spawn->candidates[spawn->next_candidate++];
        g_autoptr (GError) spawn_err = NULL;
        GSubprocess *proc = g_subprocess_newv (argv,
                G_SUBPROCESS_FLAGS_STDIN_PIPE |
                G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                &spawn_err);
        if (proc == NULL)
            continue;                        /* binary not on PATH; try the next one */

        gchar *secure_input = secure_strdup (spawn->text);
        if (secure_input == NULL) {
            g_object_unref (proc);
            continue;
        }
        g_autoptr (GBytes) input = g_bytes_new_with_free_func (
            secure_input, strlen (secure_input),
            (GDestroyNotify) gcry_free, secure_input);

        spawn->proc = proc;
        spawn->timeout_id = g_timeout_add_seconds (CLIPBOARD_TOOL_TIMEOUT_SECONDS,
                                                   on_clipboard_tool_timeout, spawn);
        g_subprocess_communicate_async (proc, input, NULL, on_clipboard_tool_done, spawn);
        return;
    }

    g_warning ("Could not copy the OTP to the clipboard: none of wl-copy, xclip "
               "or xsel is available and working");
    clipboard_spawn_free (spawn);
}


static void
copy_via_subprocess (const gchar *text)
{
    /* On Wayland the X selection tools either fail outright or only address
     * XWayland's own selection - wl-copy is the only thing that talks to the
     * compositor's data device. On X11 try xclip first and fall back to xsel,
     * since distros ship one or the other by default.
     *
     * Try every tool regardless of the guess, best guess first. Committing to
     * one branch meant that a wrong guess produced no copy at all, and an
     * XWayland session can legitimately have both working. */
    static const gchar *argv_wl[]    = { "wl-copy", NULL };
    static const gchar *argv_xclip[] = { "xclip", "-selection", "clipboard", NULL };
    static const gchar *argv_xsel[]  = { "xsel", "--clipboard", "--input", NULL };

    if (text == NULL || text[0] == '\0')
        return;

    ClipboardSpawn *spawn = g_new0 (ClipboardSpawn, 1);
    spawn->text = secure_strdup (text);
    if (spawn->text == NULL) {
        g_free (spawn);
        g_warning ("Could not copy the OTP to the clipboard: secure memory is exhausted");
        return;
    }
    spawn->generation = delivery_generation;
    if (session_is_wayland ()) {
        spawn->candidates[0] = argv_wl;
        spawn->candidates[1] = argv_xclip;
        spawn->candidates[2] = argv_xsel;
    } else {
        spawn->candidates[0] = argv_xclip;
        spawn->candidates[1] = argv_xsel;
        spawn->candidates[2] = argv_wl;
    }
    clipboard_spawn_step (spawn);
}


/* Best-effort clipboard copy on Activate/Run. We deliberately do NOT schedule
 * an auto-clear: on KDE the dominant path goes through Klipper, whose history
 * retains the OTP regardless of any setClipboardContents("") we issue (there
 * is no per-entry history removal in its D-Bus API), so a clear timer would
 * give a misleading sense of protection. Users who want the OTP to disappear
 * sooner should configure Klipper's history retention or clear it manually. */
static void
copy_to_clipboard (GDBusConnection *conn,
                   const gchar     *text,
                   gboolean         is_kde)
{
    if (!provider_access_allowed ())
        return;

    if (text == NULL || text[0] == '\0') return;
    if (is_kde) {
        copy_via_klipper (conn, text);   /* async; falls back internally */
        return;
    }
    copy_via_subprocess (text);
}


/* A notification carrying a live OTP must not be kept. Left alone, GNOME files
 * it in the message tray and shows the body on the lock screen, and KDE keeps it
 * in notification history: the code outlives its 30 second window in a place the
 * user never looks. The KRunner subtext deliberately omits the code for the same
 * reason, so the notification was the one place it escaped. Both paths below
 * mark a code notification transient and have it gone after this long. */
#define NOTIFICATION_EXPIRE_MS 5000


#ifdef IS_FLATPAK
/* Inside the sandbox the notification goes through the portal. Flathub will not
 * grant --talk-name=org.freedesktop.Notifications to an app that could use the
 * portal instead, and the portal does not need a GApplication: it works out who
 * is calling from the sandbox itself. Native installs keep the direct call,
 * because there may be no portal running at all. */
#define PORTAL_BUS_NAME     "org.freedesktop.portal.Desktop"
#define PORTAL_OBJECT_PATH  "/org/freedesktop/portal/desktop"
#define PORTAL_NOTIFICATION "org.freedesktop.portal.Notification"

/* The "transient" display hint arrived in version 2 of the interface, and older
 * portals (xdg-desktop-portal before 1.19: Ubuntu 24.04, Debian 12) do not skip
 * a key they do not know, they refuse the whole notification. So the hint only
 * goes to a portal that has said it is version 2 or later. Zero means it has not
 * answered yet, which counts as version 1. */
static guint32 portal_notification_version;
static guint   portal_notification_serial;

static void
on_portal_notification_version (GObject      *source,
                                GAsyncResult *res,
                                gpointer      user_data G_GNUC_UNUSED)
{
    g_autoptr (GVariant) reply =
        g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), res, NULL);
    if (reply == NULL) return;

    g_autoptr (GVariant) value = NULL;
    g_variant_get (reply, SP_SIG_REPLY_PROPERTIES_GET, &value);
    if (g_variant_is_of_type (value, G_VARIANT_TYPE_UINT32))
        portal_notification_version = g_variant_get_uint32 (value);
}


/* Asked once, at startup. The provider is D-Bus activated by the first search,
 * so the answer is back long before anyone can activate a result; if it is not,
 * that one notification simply goes out without the hint. */
static void
probe_portal_notification_version (void)
{
    GDBusConnection *conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
    if (conn == NULL) return;

    g_dbus_connection_call (conn, PORTAL_BUS_NAME, PORTAL_OBJECT_PATH,
                            "org.freedesktop.DBus.Properties", "Get",
                            g_variant_new (SP_SIG_PROPERTIES_GET, PORTAL_NOTIFICATION, "version"),
                            G_VARIANT_TYPE (SP_SIG_REPLY_PROPERTIES_GET),
                            G_DBUS_CALL_FLAGS_NONE, 5000, NULL,
                            on_portal_notification_version, NULL);
    g_object_unref (conn);
}


/* The portal has no expiry, and a version 1 backend drops the transient hint on
 * the floor (xdg-desktop-portal-kde is one), which would leave the code sitting
 * in the history. Withdrawing it ourselves covers every portal version. */
static gboolean
withdraw_portal_notification (gpointer user_data)
{
    const gchar *id = user_data;
    GDBusConnection *conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
    if (conn == NULL) return G_SOURCE_REMOVE;

    g_dbus_connection_call (conn, PORTAL_BUS_NAME, PORTAL_OBJECT_PATH, PORTAL_NOTIFICATION,
                            "RemoveNotification", g_variant_new (SP_SIG_REMOVE_NOTIFICATION, id),
                            NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL);
    g_object_unref (conn);
    return G_SOURCE_REMOVE;
}


static void
notify_via_portal (GDBusConnection *conn,
                   const gchar     *summary,
                   const gchar     *body,
                   gboolean         transient)
{
    /* A fresh id every time. Reusing one makes the portal update the earlier
     * notification in place, and the withdraw still pending for that earlier
     * code would then take the new one down early. */
    g_autofree gchar *id = g_strdup_printf ("otpclient-%u", ++portal_notification_serial);

    GVariantBuilder notification;
    g_variant_builder_init (&notification, G_VARIANT_TYPE ("a{sv}"));
    g_variant_builder_add (&notification, "{sv}", "title", g_variant_new_string (summary));
    g_variant_builder_add (&notification, "{sv}", "body", g_variant_new_string (body));
    if (transient && portal_notification_version >= 2) {
        const gchar *display_hint[] = { "transient", NULL };
        g_variant_builder_add (&notification, "{sv}", "display-hint",
                               g_variant_new_strv (display_hint, -1));
    }

    g_dbus_connection_call (conn, PORTAL_BUS_NAME, PORTAL_OBJECT_PATH, PORTAL_NOTIFICATION,
                            "AddNotification",
                            g_variant_new (SP_SIG_ADD_NOTIFICATION, id, &notification),
                            NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL);
    if (transient)
        g_timeout_add_full (G_PRIORITY_DEFAULT, NOTIFICATION_EXPIRE_MS,
                            withdraw_portal_notification, g_steal_pointer (&id), g_free);
}
#else
static void
notify_directly (GDBusConnection *conn,
                 const gchar     *summary,
                 const gchar     *body,
                 gboolean         transient)
{
    GVariantBuilder actions, hints;
    g_variant_builder_init (&actions, G_VARIANT_TYPE ("as"));
    g_variant_builder_init (&hints, G_VARIANT_TYPE ("a{sv}"));
    if (transient)
        g_variant_builder_add (&hints, "{sv}", "transient", g_variant_new_boolean (TRUE));

    g_dbus_connection_call (conn,
                            "org.freedesktop.Notifications",
                            "/org/freedesktop/Notifications",
                            "org.freedesktop.Notifications",
                            "Notify",
                            g_variant_new (SP_SIG_NOTIFY,
                                           "OTPClient", (guint32)0,
                                           "com.github.paolostivanin.OTPClient",
                                           summary, body,
                                           &actions, &hints, (gint32)NOTIFICATION_EXPIRE_MS),
                            NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, NULL, NULL);
}
#endif


/* Fire-and-forget: we never look at the notification id, and waiting on the
 * reply would park the provider's only thread on whatever the notification
 * daemon is doing. */
static void
send_notification_full (const gchar *summary,
                        const gchar *body,
                        gboolean     transient)
{
    GDBusConnection *conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
    if (conn == NULL) return;

#ifdef IS_FLATPAK
    notify_via_portal (conn, summary, body, transient);
#else
    notify_directly (conn, summary, body, transient);
#endif
    g_object_unref (conn);
}


static void
send_notification (const gchar *label,
                   const gchar *otp_value)
{
    if (!provider_access_allowed ())
        return;

    if (!otp_value) return;
    gchar *body = g_strdup_printf ("Your code for %s is: %s",
                                    label ? label : "Account", otp_value);
    send_notification_full ("OTP Token", body, TRUE);
    sensitive_g_free (body);
}


/* Activation that produces no code used to end in silence: no toast, no error,
 * no journal line. Tell the user why instead, so "the keyring has no password
 * for this database" stops looking like "the search provider ignored me". */
static void
send_failure_notification (const gchar *label,
                           const gchar *reason)
{
    if (!provider_access_allowed ())
        return;

    g_autofree gchar *body = g_strdup_printf ("%s: %s", label ? label : "Account", reason);
    send_notification_full ("OTPClient could not produce a code", body, FALSE);
}


/* Both frontends now answer from a callback rather than from the handler: the
 * keyring lookup underneath is asynchronous, and GDBus keeps the invocation
 * alive until someone returns a value on it. */
typedef struct {
    GDBusMethodInvocation *inv;              /* borrowed until returned */
    gchar                **stripped;         /* post-keyword terms, owned */
    gchar                 *normalized_query; /* owned */
} QueryJob;

typedef struct {
    GDBusMethodInvocation *inv;
    GDBusConnection       *conn;
    gchar                 *db_path;
    gchar                 *label;
    gchar                 *token_identity;
    gsize                  json_index;
    gboolean               is_kde;
    guint64                generation;
    /* Bounds the keyring lookup: a wedged Secret Service used to leave this
     * job and its unanswered GDBusMethodInvocation leaked forever. See the
     * EntriesReload guard for the same pattern. */
    GCancellable *keyring_cancellable;
    guint         keyring_deadline_id;
    gboolean      keyring_timed_out;
} ActivationJob;


static QueryJob *
query_job_new (GDBusMethodInvocation *inv,
               gchar                **stripped)
{
    QueryJob *job = g_new0 (QueryJob, 1);
    job->inv = inv;
    job->stripped = stripped;
    job->normalized_query = normalize_terms (stripped);
    return job;
}


static void
query_job_free (QueryJob *job)
{
    g_strfreev (job->stripped);
    g_free (job->normalized_query);
    g_free (job);
}


static void
activation_job_free (ActivationJob *job)
{
    if (job->keyring_deadline_id != 0) {
        g_source_remove (job->keyring_deadline_id);
        job->keyring_deadline_id = 0;
    }
    g_clear_object (&job->keyring_cancellable);
    g_clear_object (&job->conn);
    g_free (job->db_path);
    g_free (job->label);
    g_free (job->token_identity);
    g_free (job);
}


static gboolean
on_activation_keyring_deadline (gpointer user_data)
{
    ActivationJob *job = user_data;
    job->keyring_deadline_id = 0;
    job->keyring_timed_out = TRUE;
    g_warning ("Search provider: keyring lookup for %s did not answer within %d s; giving up.",
               job->db_path != NULL ? job->db_path : "(unknown)",
               KEYRING_LOOKUP_DEADLINE_SECONDS);
    g_cancellable_cancel (job->keyring_cancellable);
    return G_SOURCE_REMOVE;
}


static void
on_activation_password (GObject      *source G_GNUC_UNUSED,
                        GAsyncResult *res,
                        gpointer      user_data)
{
    ActivationJob *job = user_data;
    if (job->keyring_deadline_id != 0) {
        g_source_remove (job->keyring_deadline_id);
        job->keyring_deadline_id = 0;
    }
    g_clear_object (&job->keyring_cancellable);
    g_autoptr (GError) err = NULL;
    gchar *pwd = otpclient_secret_lookup_with_legacy_fallback_finish (res, NULL, &err);
    g_autofree gchar *failure = NULL;
    gchar *otp = NULL;

    if (job->generation != delivery_generation || !provider_access_allowed ()) {
        /* The provider was switched off between the click and the lookup.
         * Deliver nothing, and say nothing either: the user just turned it off. */
    } else if (job->keyring_timed_out) {
        /* The deadline fired and cancelled the lookup: fail the delivery
         * instead of leaving the invocation unanswered forever. */
        failure = g_strdup ("The keyring did not answer in time. "
                            "Unlock the database once in OTPClient and try again.");
    } else if (err != NULL) {
        g_warning ("Search provider: secret service lookup failed for %s: %s",
                   job->db_path, err->message);
        failure = g_strdup ("The keyring could not be reached.");
    } else if (pwd == NULL) {
        g_warning ("Search provider: no keyring entry for '%s'", job->db_path);
        failure = g_strdup ("No password for this database is stored in the keyring. "
                            "Unlock it once in OTPClient.");
    } else {
        otp = compute_otp_with_password (job->db_path, job->token_identity,
                                         job->json_index, pwd, &failure);
    }
    if (pwd != NULL)
        secret_password_free (pwd);

    if (otp != NULL) {
        copy_to_clipboard (job->conn, otp, job->is_kde);
        send_notification (job->label, otp);
        sensitive_secure_free (otp);
    } else if (failure != NULL) {
        send_failure_notification (job->label, failure);
    }

    g_dbus_method_invocation_return_value (job->inv, NULL);
    activation_job_free (job);
}


static void
activation_job_start (GDBusMethodInvocation      *inv,
                      GDBusConnection            *conn,
                      const ActivationCapability *cap,
                      gboolean                    is_kde)
{
    ActivationJob *job = g_new0 (ActivationJob, 1);
    job->inv = inv;
    job->conn = conn != NULL ? g_object_ref (conn) : NULL;
    job->db_path = g_strdup (cap->db_path);
    job->label = g_strdup (cap->label);
    job->token_identity = g_strdup (cap->token_identity);
    job->json_index = cap->json_index;
    job->is_kde = is_kde;
    job->generation = delivery_generation;

    /* Issue #448: v4 fallback so a v4 upgrader who has not opened the GUI yet
     * still gets OTP values from the search provider. The lookup is bounded:
     * a wedged Secret Service must not leave the invocation unanswered. */
    job->keyring_timed_out = FALSE;
    job->keyring_cancellable = g_cancellable_new ();
    job->keyring_deadline_id = g_timeout_add_seconds (
        KEYRING_LOOKUP_DEADLINE_SECONDS, on_activation_keyring_deadline, job);
    otpclient_secret_lookup_with_legacy_fallback_async (job->db_path,
                                                        job->keyring_cancellable,
                                                        on_activation_password, job);
}


/* An id that matches no live capability is either expired (the TTL is 30 s) or
 * forged. Log it, but deliberately do not notify: unlike every other failure
 * here, this one is reachable without ever holding a valid id, so a notification
 * would hand any process on the session bus a way to pop toasts at the user. */
static void
log_unknown_activation_id (void)
{
    g_info ("Search provider: activation for an unknown or expired result id, ignoring.");
}


static void
on_gnome_query_entries (GObject      *source G_GNUC_UNUSED,
                        GAsyncResult *res,
                        gpointer      user_data)
{
    QueryJob *job = user_data;
    g_autoptr (GPtrArray) entries = get_entries_finish (res);
    GVariantBuilder builder;

    g_variant_builder_init (&builder, G_VARIANT_TYPE ("as"));
    gsize stripped_len = g_strv_length (job->stripped);
    for (guint i = 0; entries != NULL && i < entries->len; i++) {
        OtpSearchEntry *e = g_ptr_array_index (entries, i);
        if (!entry_matches_terms (e, job->stripped, stripped_len))
            continue;
        g_autofree gchar *cap = issue_activation_capability (
            g_dbus_method_invocation_get_sender (job->inv), job->normalized_query, e);
        if (cap != NULL)
            g_variant_builder_add (&builder, "s", cap);
    }
    g_dbus_method_invocation_return_value (job->inv, g_variant_new (SP_SIG_REPLY_RESULT_IDS, &builder));
    query_job_free (job);
}


static void
handle_gnome_call (GDBusConnection       *conn,
                   const gchar           *sender,
                   const gchar           *path,
                   const gchar           *iface,
                   const gchar           *method,
                   GVariant              *params,
                   GDBusMethodInvocation *inv,
                   gpointer               data)
{
    (void)sender; (void)path; (void)iface; (void)data;
    if (!provider_access_allowed ()) {
        return_disabled_result (method, inv);
        return;
    }
    g_last_activity_us = g_get_monotonic_time ();

    if (g_strcmp0 (method, "GetInitialResultSet") == 0 || g_strcmp0 (method, "GetSubsearchResultSet") == 0) {
        gchar **terms;
        if (g_strcmp0 (method, "GetInitialResultSet") == 0) {
            g_variant_get (params, SP_SIG_GET_INITIAL_RESULT_SET, &terms);
        } else {
            gchar **prev_results = NULL;
            g_variant_get (params, SP_SIG_GET_SUBSEARCH_RESULT_SET, &prev_results, &terms);
            g_strfreev (prev_results);
        }
        gchar **stripped = NULL;
        gboolean matched = strip_keyword_or_skip (terms, &stripped);
        g_strfreev (terms);
        if (!matched) {
            /* Nothing to look up: answer straight away rather than touching the
             * keyring for a query that is not addressed to us. */
            GVariantBuilder builder;
            g_variant_builder_init (&builder, G_VARIANT_TYPE ("as"));
            g_dbus_method_invocation_return_value (inv, g_variant_new (SP_SIG_REPLY_RESULT_IDS, &builder));
            return;
        }
        get_entries_async (on_gnome_query_entries, query_job_new (inv, stripped));
    } else if (g_strcmp0 (method, "GetResultMetas") == 0) {
        gchar **ids;
        g_variant_get (params, SP_SIG_GET_RESULT_METAS, &ids);
        GVariantBuilder builder;
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("aa{sv}"));
        for (gsize j = 0; ids[j]; j++) {
            ActivationCapability *cap = lookup_activation_capability (
                ids[j], g_dbus_method_invocation_get_sender (inv));
            if (cap != NULL) {
                GVariantBuilder meta;
                g_variant_builder_init (&meta, G_VARIANT_TYPE ("a{sv}"));
                g_variant_builder_add (&meta, "{sv}", "id", g_variant_new_string (cap->id));
                g_variant_builder_add (&meta, "{sv}", "name", g_variant_new_string (cap->label));
                g_variant_builder_add (&meta, "{sv}", "description", g_variant_new_string (cap->label));
                g_variant_builder_add (&meta, "{sv}", "icon", g_variant_new_string ("com.github.paolostivanin.OTPClient"));
                g_variant_builder_add_value (&builder, g_variant_builder_end (&meta));
            }
        }
        g_dbus_method_invocation_return_value (inv, g_variant_new (SP_SIG_REPLY_RESULT_METAS, &builder));
        g_strfreev (ids);
    } else if (g_strcmp0 (method, "ActivateResult") == 0) {
        // Refuse activation entirely when the keyword gate is disabled, so a
        // caller who guesses an id can't trigger OTP delivery without ever
        // having satisfied the keyword filter.
        if (g_keyword_fold == NULL || g_keyword_fold[0] == '\0') {
            g_dbus_method_invocation_return_value (inv, NULL);
            return;
        }
        /* Per-sender token bucket: a hostile session-bus peer could
         * otherwise spam ActivateResult to burn Argon2id CPU/memory and
         * surface a flood of notifications carrying live OTPs. The bucket
         * is generous enough for legit double-clicks but caps sustained
         * abuse at RATE_REFILL_PER_SEC. */
        if (!rate_bucket_consume (g_dbus_method_invocation_get_sender (inv))) {
            g_dbus_method_invocation_return_value (inv, NULL);
            return;
        }
        /* Initialise both: if the format string below is ever wrong again,
         * g_variant_get fails its precondition and returns without touching a
         * single out-parameter, and `id` would then be read uninitialised. */
        const gchar *id = NULL;
        gchar **terms = NULL;
        /* No spaces inside the tuple. A stray one makes this an invalid format
         * string, which g_variant_get rejects with a CRITICAL and a silent
         * no-op rather than a compile error, and activation quietly does
         * nothing at all. */
        g_variant_get (params, SP_SIG_ACTIVATE_RESULT, &id, &terms, NULL);
        g_auto(GStrv) stripped = NULL;
        g_autofree gchar *normalized_query = NULL;
        if (strip_keyword_or_skip (terms, &stripped))
            normalized_query = normalize_terms (stripped);
        ActivationCapability *cap = (normalized_query != NULL)
            ? consume_activation_capability (id, g_dbus_method_invocation_get_sender (inv), normalized_query)
            : NULL;
        g_strfreev (terms);
        if (cap == NULL) {
            log_unknown_activation_id ();
            g_dbus_method_invocation_return_value (inv, NULL);
            return;
        }
        activation_job_start (inv, conn, cap, FALSE);
        activation_capability_free (cap);
    } else {
        g_dbus_method_invocation_return_value (inv, NULL);
    }
}


static void
on_krunner_query_entries (GObject      *source G_GNUC_UNUSED,
                          GAsyncResult *res,
                          gpointer      user_data)
{
    QueryJob *job = user_data;
    g_autoptr (GPtrArray) entries = get_entries_finish (res);
    GVariantBuilder builder;

    g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sssida{sv})"));
    gsize stripped_len = g_strv_length (job->stripped);
    for (guint i = 0; entries != NULL && i < entries->len; i++) {
        OtpSearchEntry *e = g_ptr_array_index (entries, i);
        if (!entry_matches_terms (e, job->stripped, stripped_len)) continue;
        g_autofree gchar *cap = issue_activation_capability (
            g_dbus_method_invocation_get_sender (job->inv), job->normalized_query, e);
        if (cap == NULL)
            continue;
        GVariantBuilder props;
        g_variant_builder_init (&props, G_VARIANT_TYPE ("a{sv}"));
        // Deliberately do NOT include the OTP value in the subtitle:
        // any process on the session bus can poll Match. The code is
        // only handed out via Run, where the user sees a notification.
        g_autofree gchar *sub = NULL;
        if (e->db_name != NULL && e->db_name[0] != '\0')
            sub = (e->issuer && *e->issuer)
                ? g_strdup_printf ("%s - %s", e->db_name, e->issuer)
                : g_strdup (e->db_name);
        else
            sub = g_strdup (e->issuer ? e->issuer : "");
        g_variant_builder_add (&props, "{sv}", "subtext", g_variant_new_string (sub));
        g_variant_builder_add (&props, "{sv}", "category", g_variant_new_string ("OTPClient"));
        g_variant_builder_add (&builder, "(sssida{sv})",
                               cap, e->label,
                               "com.github.paolostivanin.OTPClient",
                               (gint32)0, (gdouble)1.0, &props);
    }
    GVariant *res_value = g_variant_builder_end (&builder);
    g_dbus_method_invocation_return_value (job->inv, g_variant_new_tuple (&res_value, 1));
    query_job_free (job);
}


static void
handle_krunner_call (GDBusConnection       *conn,
                     const gchar           *sender,
                     const gchar           *path,
                     const gchar           *iface,
                     const gchar           *method,
                     GVariant              *params,
                     GDBusMethodInvocation *inv,
                     gpointer               data)
{
    (void)sender; (void)path; (void)iface; (void)data;
    if (!provider_access_allowed ()) {
        return_disabled_result (method, inv);
        return;
    }
    g_last_activity_us = g_get_monotonic_time ();

    if (g_strcmp0 (method, "Match") == 0) {
        const gchar *query;
        g_variant_get (params, SP_SIG_KRUNNER_MATCH, &query);
        g_auto(GStrv) terms = (query != NULL && *query != '\0')
            ? g_strsplit_set (query, " \t", -1)
            : NULL;
        gchar **stripped = NULL;
        if (!strip_keyword_or_skip (terms, &stripped)) {
            /* Not our keyword: answer an empty match list without going near the
             * keyring. KRunner polls Match on every keystroke. */
            GVariantBuilder builder;
            g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sssida{sv})"));
            GVariant *res = g_variant_builder_end (&builder);
            g_dbus_method_invocation_return_value (inv, g_variant_new_tuple (&res, 1));
            return;
        }
        get_entries_async (on_krunner_query_entries, query_job_new (inv, stripped));
    } else if (g_strcmp0 (method, "Run") == 0) {
        // Same gate as the GNOME ActivateResult path: no keyword set means
        // the provider refuses to deliver codes, even via id lookup.
        if (g_keyword_fold == NULL || g_keyword_fold[0] == '\0') {
            g_dbus_method_invocation_return_value (inv, NULL);
            return;
        }
        /* Same per-sender token bucket as ActivateResult; see comment there. */
        if (!rate_bucket_consume (g_dbus_method_invocation_get_sender (inv))) {
            g_dbus_method_invocation_return_value (inv, NULL);
            return;
        }
        const gchar *id = NULL;
        g_variant_get (params, SP_SIG_KRUNNER_RUN, &id, NULL);
        ActivationCapability *cap = consume_activation_capability (
            id, g_dbus_method_invocation_get_sender (inv), NULL);
        if (cap == NULL) {
            log_unknown_activation_id ();
            g_dbus_method_invocation_return_value (inv, NULL);
            return;
        }
        activation_job_start (inv, conn, cap, TRUE);
        activation_capability_free (cap);
    } else if (g_strcmp0 (method, "Actions") == 0) {
        GVariant *empty = g_variant_new_array (G_VARIANT_TYPE ("(sss)"), NULL, 0);
        g_dbus_method_invocation_return_value (inv, g_variant_new_tuple (&empty, 1));
    }
}


static const GDBusInterfaceVTable k_vtable = { handle_krunner_call, NULL, NULL, {0} };
static const GDBusInterfaceVTable g_vtable = { handle_gnome_call, NULL, NULL, {0} };


static void
on_krunner_bus_acquired (GDBusConnection *conn,
                         const gchar     *name G_GNUC_UNUSED,
                         gpointer         data G_GNUC_UNUSED)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml (krunner_introspection_xml, &error);
    if (node)
        g_dbus_connection_register_object (conn, KRUNNER_PATH, node->interfaces[0], &k_vtable, NULL, NULL, NULL);
}


static void
on_gnome_bus_acquired (GDBusConnection *conn,
                       const gchar     *name G_GNUC_UNUSED,
                       gpointer         data G_GNUC_UNUSED)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml (gnome_introspection_xml, &error);
    if (node)
        g_dbus_connection_register_object (conn, GNOME_PATH, node->interfaces[0], &g_vtable, NULL, NULL, NULL);
}


static GMainLoop *main_loop = NULL;


static void
on_name_lost (GDBusConnection *conn G_GNUC_UNUSED,
              const gchar     *name,
              gpointer         data G_GNUC_UNUSED)
{
    g_printerr ("Lost (or failed to acquire) D-Bus name '%s'. Is another instance running?\n", name);
    if (main_loop != NULL)
        g_main_loop_quit (main_loop);
}


int
main (int    argc,
      char **argv)
{
    gboolean force_kde = FALSE, force_gnome = FALSE;
    for (int i = 1; i < argc; i++) {
        if (g_strcmp0 (argv[i], "--kde") == 0) force_kde = TRUE;
        else if (g_strcmp0 (argv[i], "--gnome") == 0) force_gnome = TRUE;
    }

    /* Deliberately not "exit when the provider is disabled". Both service files
     * are D-Bus activated, so exiting before owning the name makes the bus
     * answer Spawn.ChildExited to GNOME Shell and KRunner on every single
     * search for as long as the feature stays off. Own the name and serve the
     * correctly-typed empty results that return_disabled_result already builds;
     * provider_access_allowed gates every handler, and the settings-changed
     * handler means flipping the switch back on self-heals without a restart. */
    gboolean enabled_at_startup = gsettings_common_get_search_provider_enabled ();

    load_keyword_config ();
    provider_settings = gsettings_common_get_settings ();
    if (provider_settings != NULL)
        g_signal_connect (provider_settings, "changed", G_CALLBACK (on_provider_settings_changed), NULL);

    if (!force_kde && !force_gnome) {
        const gchar *desktop = g_getenv ("XDG_CURRENT_DESKTOP");
        if (desktop) {
            g_autofree gchar *dl = g_ascii_strdown (desktop, -1);
            if (g_strstr_len (dl, -1, "kde") || g_strstr_len (dl, -1, "plasma"))
                force_kde = TRUE;
            else if (g_strstr_len (dl, -1, "gnome"))
                force_gnome = TRUE;
        }
    }
    if (!force_kde && !force_gnome)
        return 0;

    /* When the provider is enabled, fail loudly and immediately if the crypto
     * setup is broken rather than turning every query into a warning. When it is
     * disabled there is nothing to decrypt, so skip it: the secure-memory pool
     * is mlocked, and a switched-off provider should not hold locked pages for
     * the life of the session. ensure_crypto_ready does it on first use if the
     * setting is turned on later. */
    if (enabled_at_startup && !ensure_crypto_ready ())
        return 1;

    main_loop = g_main_loop_new (NULL, FALSE);
    if (force_kde)
        g_bus_own_name (G_BUS_TYPE_SESSION, KRUNNER_BUS, G_BUS_NAME_OWNER_FLAGS_NONE,
                        on_krunner_bus_acquired, NULL, on_name_lost, NULL, NULL);
    if (force_gnome)
        g_bus_own_name (G_BUS_TYPE_SESSION, GNOME_BUS, G_BUS_NAME_OWNER_FLAGS_NONE,
                        on_gnome_bus_acquired, NULL, on_name_lost, NULL, NULL);
#ifdef IS_FLATPAK
    probe_portal_notification_version ();
#endif
    g_last_activity_us = g_get_monotonic_time ();
    g_timeout_add_seconds (60, idle_wipe_check, NULL);
    g_main_loop_run (main_loop);
    clear_file_monitors ();
    g_clear_pointer (&cached_entries, g_ptr_array_unref);
    /* Wipe derived keys + per-sender state on shutdown. The kdf_cache entry
     * destroy callback explicit_bzero's the derived key before gcry_free. */
    kdf_cache_clear ();
    rate_buckets_clear ();
    activation_capabilities_clear ();
    g_clear_object (&provider_settings);
    g_clear_pointer (&g_keyword, g_free);
    g_clear_pointer (&g_keyword_fold, g_free);
    g_main_loop_unref (main_loop);
    return 0;
}
