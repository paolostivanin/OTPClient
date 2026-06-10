#define _DEFAULT_SOURCE
#include <gio/gio.h>
#include <glib.h>
#include <jansson.h>
#include <libsecret/secret.h>
#include <gcrypt.h>
#include <cotp.h>
#include <string.h>
#include <time.h>

#include "../common/common.h"
#include "../common/db-common.h"
#include "../common/file-size.h"
#include "../common/secret-schema.h"
#include "../common/gsettings-common.h"

#define KRUNNER_BUS "com.github.paolostivanin.OTPClient.KRunner"
#define KRUNNER_PATH "/com/github/paolostivanin/OTPClient/KRunner"
#define GNOME_BUS "com.github.paolostivanin.OTPClient.SearchProvider"
#define GNOME_PATH "/com/github/paolostivanin/OTPClient/SearchProvider"

static gint32 global_max_file_size = 0;

/* Cache TTL is a fallback: file monitors invalidate the cache eagerly when a
 * DB file is touched on disk. The TTL covers the case where the on-disk DB
 * is unchanged but a HOTP counter / secret service value moved underneath us. */
#define CACHE_TTL_SECONDS 60

static GPtrArray  *cached_entries = NULL;
static gint64      cached_at = 0;
/* path (gchar*) → GFileMonitor*. Diffed across reloads so monitors for
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
 * when the DB file changes (password change → new salt → cache miss anyway,
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
 * entries cache already absorbs them; only the OTP-yielding paths are. */
#define RATE_BUCKET_MAX     10.0
#define RATE_REFILL_PER_SEC  5.0
typedef struct {
    gdouble  tokens;
    gint64   last_refill_us;          /* g_get_monotonic_time() at last update */
} RateBucket;

static GHashTable *g_rate_buckets = NULL;

/* Trigger keyword loaded once at startup. The daemon ignores any query whose
 * first whitespace-separated token doesn't equal g_keyword (case-insensitive).
 * An empty keyword disables the filter and falls back to plain substring
 * matching. Changes to the GSettings key only take effect after the daemon
 * is restarted. */
static gchar *g_keyword = NULL;
static gchar *g_keyword_fold = NULL;

typedef struct otp_search_entry_t {
    gchar *id;
    gchar *label;
    gchar *issuer;
    gchar *db_name;
    gchar *db_path;        /* needed to recompute OTP on Run/Activate */
    gsize  json_index;     /* position of the token within the DB's JSON array */
    /* Pre-folded copies for entry_matches_terms — avoid casefolding per query. */
    gchar *label_fold;
    gchar *issuer_fold;
} OtpSearchEntry;

static void otp_search_entry_free (OtpSearchEntry *entry);
static GPtrArray *load_entries_uncached (void);
static GPtrArray *get_entries (void);
static gboolean entry_matches_terms (const OtpSearchEntry *entry, gchar **terms, gsize terms_len);
static gchar *get_entry_otp_value (json_t *obj);
static gchar *compute_otp_for_entry (const OtpSearchEntry *entry);
static void send_notification (const gchar *label, const gchar *otp_value);
static void copy_to_clipboard (const gchar *text, gboolean is_kde);
static void clear_file_monitors (void);
static void sync_file_monitors (GPtrArray *desired_paths);
static void on_db_file_changed (GFileMonitor *monitor, GFile *file, GFile *other,
                                GFileMonitorEvent event, gpointer user_data);
static gboolean prewarm_cache (gpointer user_data);
static void kdf_cache_entry_free (KdfCacheEntry *entry);
static void kdf_cache_invalidate_path (const gchar *db_path);
static void kdf_cache_clear (void);
static void kdf_cache_apply_to_db_data (DatabaseData *db_data, const gchar *db_path);
static void kdf_cache_capture_from_db_data (const DatabaseData *db_data, const gchar *db_path);
static gboolean rate_bucket_consume (const gchar *sender);
static void rate_buckets_clear (void);

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
 * RATE_BUCKET_MAX. A NULL sender (peer-to-peer connection without a name)
 * is bucketed under a fixed key so it can't bypass the limiter by being
 * unidentifiable. */
static gboolean
rate_bucket_consume (const gchar *sender)
{
    if (g_rate_buckets == NULL)
        g_rate_buckets = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, g_free);

    const gchar *key = (sender != NULL && sender[0] != '\0') ? sender : ":anon";
    gint64 now = g_get_monotonic_time ();

    RateBucket *bucket = g_hash_table_lookup (g_rate_buckets, key);
    if (bucket == NULL) {
        bucket = g_new0 (RateBucket, 1);
        bucket->tokens = RATE_BUCKET_MAX;
        bucket->last_refill_us = now;
        g_hash_table_insert (g_rate_buckets, g_strdup (key), bucket);
    } else {
        gdouble elapsed_sec = (gdouble) (now - bucket->last_refill_us) / 1.0e6;
        if (elapsed_sec > 0) {
            bucket->tokens += elapsed_sec * RATE_REFILL_PER_SEC;
            if (bucket->tokens > RATE_BUCKET_MAX)
                bucket->tokens = RATE_BUCKET_MAX;
            bucket->last_refill_us = now;
        }
    }

    if (bucket->tokens < 1.0)
        return FALSE;
    bucket->tokens -= 1.0;
    return TRUE;
}


static void
rate_buckets_clear (void)
{
    if (g_rate_buckets == NULL)
        return;
    g_hash_table_destroy (g_rate_buckets);
    g_rate_buckets = NULL;
}


static gchar *
get_entry_otp_value (json_t *obj)
{
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
        glong current_ts = time (NULL);
        if (issuer != NULL && g_ascii_strcasecmp (issuer, "steam") == 0) {
            token = get_steam_totp_at (secret, current_ts, period, &cotp_err);
        } else {
            token = get_totp_at (secret, current_ts, digits, period, algo, &cotp_err);
        }
    } else if (g_ascii_strcasecmp (type, "HOTP") == 0) {
        gint64 counter = json_integer_value (json_object_get (obj, "counter"));
        token = get_hotp (secret, counter, digits, algo, &cotp_err);
    }

    if (token == NULL) return NULL;
    gchar *result = g_strdup (token);
    g_free (token);
    return result;
}


static void
load_entries_from_db (GPtrArray   *entries,
                      const gchar *db_path,
                      const gchar *db_name,
                      guint        db_index)
{
    if (!gsettings_common_get_use_secret_service ())
        return;

    /* Issue #446: surface broken-keyring errors via a warning instead of
     * silently returning. Don't mutate GSettings here, the search provider
     * is a passive consumer; the GUI app owns the setting.
     * Issue #448: try the v4 "main_pwd" entry too when the db_path-keyed
     * lookup misses, so users who upgraded but have not opened the GUI yet
     * still get search hits. No cleanup here, the GUI's first launch is
     * what migrates and clears the legacy entry. */
    GError *ss_err = NULL;
    gchar *pwd = otpclient_secret_lookup_with_legacy_fallback (db_path, NULL, &ss_err);
    if (ss_err != NULL) {
        g_warning ("Search provider: secret service lookup failed for %s: %s",
                   db_path, ss_err->message);
        g_clear_error (&ss_err);
        return;
    }
    if (pwd == NULL)
        return;

    DatabaseData *db_data = database_data_new (db_path, global_max_file_size);
    db_data->key = secure_strdup (pwd);
    secret_password_free (pwd);

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

    kdf_cache_capture_from_db_data (db_data, db_path);

    gsize index;
    json_t *obj;
    json_array_foreach (db_data->in_memory_json_data, index, obj)
    {
        const gchar *label = json_string_value (json_object_get (obj, "label"));
        if (label == NULL) continue;
        const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
        OtpSearchEntry *entry = g_new0 (OtpSearchEntry, 1);
        entry->id = g_strdup_printf ("%u:%" G_GSIZE_FORMAT, db_index, index);
        entry->label = g_strdup (label);
        entry->issuer = g_strdup (issuer ? issuer : "");
        entry->db_name = g_strdup (db_name);
        entry->db_path = g_strdup (db_path);
        entry->json_index = index;
        /* Pre-casefold for entry_matches_terms — done once at load instead of
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


static GPtrArray *
load_entries_uncached (void)
{
    GPtrArray *entries = g_ptr_array_new_with_free_func ((GDestroyNotify) otp_search_entry_free);

    /* Collect the desired set of paths, then diff our current monitor set
     * against it — see sync_file_monitors. */
    g_autoptr (GPtrArray) desired_paths = g_ptr_array_new ();

    g_autoptr (GPtrArray) db_list = gsettings_common_get_db_list ();
    g_autofree gchar *fallback_path = NULL;
    if (db_list != NULL && db_list->len > 0)
    {
        for (guint i = 0; i < db_list->len; i++)
        {
            DbListEntry *dbe = g_ptr_array_index (db_list, i);
            if (dbe->path != NULL)
                g_ptr_array_add (desired_paths, dbe->path);
        }
    }
    else
    {
        fallback_path = gsettings_common_get_db_path ();
        if (fallback_path != NULL)
            g_ptr_array_add (desired_paths, fallback_path);
    }

    sync_file_monitors (desired_paths);

    if (db_list != NULL && db_list->len > 0)
    {
        for (guint i = 0; i < db_list->len; i++)
        {
            DbListEntry *dbe = g_ptr_array_index (db_list, i);
            load_entries_from_db (entries, dbe->path, dbe->name, i);
        }
    }
    else if (fallback_path != NULL)
    {
        load_entries_from_db (entries, fallback_path, NULL, 0);
    }

    return entries;
}


static GPtrArray *
get_entries (void)
{
    gint64 now = time (NULL);
    if (cached_entries != NULL && cached_at != 0 && (now - cached_at) < CACHE_TTL_SECONDS)
        return cached_entries;

    if (cached_entries != NULL)
        g_ptr_array_free (cached_entries, TRUE);

    cached_entries = load_entries_uncached ();
    cached_at = now;
    return cached_entries;
}


static gboolean
prewarm_cache (gpointer user_data G_GNUC_UNUSED)
{
    /* Run once on idle so the first user query doesn't pay the Argon2id
     * cost interactively. Failures are silently ignored — they'll be
     * retried on the next query. */
    (void) get_entries ();
    return G_SOURCE_REMOVE;
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


static gchar *
compute_otp_for_entry (const OtpSearchEntry *entry)
{
    /* Open, decrypt, look up the JSON object, compute the OTP, then wipe
     * everything. The Argon2id derivation pays the user-visible latency, but
     * the cached derived key in db_data->cached_derived_key + the file
     * monitor + the 60 s entry cache mean this only happens on the first Run
     * after the DB has changed. The trade vs caching the OTP value: heap
     * inspection of the daemon never reveals an active OTP. */
    if (entry == NULL || entry->db_path == NULL) return NULL;
    if (!gsettings_common_get_use_secret_service ()) return NULL;

    GError *ss_err = NULL;
    /* Issue #448: v4 fallback so a v4 upgrader who has not opened the GUI
     * yet still gets OTP values from the search provider. */
    gchar *pwd = otpclient_secret_lookup_with_legacy_fallback (entry->db_path, NULL, &ss_err);
    if (ss_err != NULL) {
        g_warning ("Search provider: secret service lookup failed: %s", ss_err->message);
        g_clear_error (&ss_err);
        return NULL;
    }
    if (pwd == NULL) return NULL;

    DatabaseData *db_data = database_data_new (entry->db_path, global_max_file_size);
    db_data->key = secure_strdup (pwd);
    secret_password_free (pwd);

    /* Pre-load the cached Argon2id derived key (if any) before load_db so
     * try_decrypt_v2's salt+pwd_hash lookup hits and skips the 150-300 ms
     * derivation. A mismatch (changed password) falls through to a fresh
     * derive and overwrites the cache below. */
    kdf_cache_apply_to_db_data (db_data, entry->db_path);

    GError *err = NULL;
    load_db (db_data, &err);
    gchar *otp = NULL;
    if (err == NULL && db_data->in_memory_json_data != NULL) {
        json_t *obj = json_array_get (db_data->in_memory_json_data, entry->json_index);
        if (obj != NULL)
            otp = get_entry_otp_value (obj);
        /* try_decrypt_v2 populates db_data->cached_* on success; persist
         * those into g_kdf_cache so the next call hits. Capturing only on
         * success keeps a wrong-password attempt from poisoning the cache. */
        kdf_cache_capture_from_db_data (db_data, entry->db_path);
    }
    if (err != NULL) g_clear_error (&err);

    database_data_free (db_data);

    return otp;
}


static gboolean
copy_via_klipper (const gchar *text)
{
    g_autoptr (GDBusConnection) conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
    if (conn == NULL) return FALSE;
    g_autoptr (GVariant) result = g_dbus_connection_call_sync (conn,
            "org.kde.klipper", "/klipper", "org.kde.klipper.klipper",
            "setClipboardContents", g_variant_new ("(s)", text),
            NULL, G_DBUS_CALL_FLAGS_NONE, 1000, NULL, NULL);
    return result != NULL;
}


static gboolean
copy_via_subprocess (const gchar *text)
{
    /* On Wayland the X selection tools either fail outright or only address
     * XWayland's own selection — wl-copy is the only thing that talks to the
     * compositor's data device. On X11 (or unknown sessions) try xclip first
     * and fall back to xsel since distros ship one or the other by default. */
    const gchar *session = g_getenv ("XDG_SESSION_TYPE");
    gboolean is_wayland = (session != NULL && g_ascii_strcasecmp (session, "wayland") == 0);
    const gchar *argv_wl[]    = { "wl-copy", NULL };
    const gchar *argv_xclip[] = { "xclip", "-selection", "clipboard", NULL };
    const gchar *argv_xsel[]  = { "xsel", "--clipboard", "--input", NULL };
    const gchar **candidates[2] = { NULL, NULL };
    int n_candidates = 0;
    if (is_wayland) {
        candidates[n_candidates++] = argv_wl;
    } else {
        candidates[n_candidates++] = argv_xclip;
        candidates[n_candidates++] = argv_xsel;
    }
    for (int i = 0; i < n_candidates; i++) {
        g_autoptr (GSubprocess) proc = g_subprocess_newv (candidates[i],
                G_SUBPROCESS_FLAGS_STDIN_PIPE |
                G_SUBPROCESS_FLAGS_STDOUT_SILENCE |
                G_SUBPROCESS_FLAGS_STDERR_SILENCE,
                NULL);
        if (proc == NULL) continue;          /* binary not on PATH; try the next one */
        g_autoptr (GBytes) input = g_bytes_new (text, strlen (text));
        if (g_subprocess_communicate (proc, input, NULL, NULL, NULL, NULL))
            return TRUE;
    }
    return FALSE;
}


/* Best-effort clipboard copy on Activate/Run. We deliberately do NOT schedule
 * an auto-clear: on KDE the dominant path goes through Klipper, whose history
 * retains the OTP regardless of any setClipboardContents("") we issue (there
 * is no per-entry history removal in its D-Bus API), so a clear timer would
 * give a misleading sense of protection. Users who want the OTP to disappear
 * sooner should configure Klipper's history retention or clear it manually. */
static void
copy_to_clipboard (const gchar *text,
                   gboolean     is_kde)
{
    if (text == NULL || text[0] == '\0') return;
    if (is_kde && copy_via_klipper (text)) return;
    copy_via_subprocess (text);
}


static void
send_notification (const gchar *label,
                   const gchar *otp_value)
{
    if (!otp_value) return;
    GDBusConnection *conn = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, NULL);
    if (!conn) return;

    g_autofree gchar *body = g_strdup_printf ("Your code for %s is: %s",
                                               label ? label : "Account", otp_value);
    GVariantBuilder actions, hints;
    g_variant_builder_init (&actions, G_VARIANT_TYPE ("as"));
    g_variant_builder_init (&hints, G_VARIANT_TYPE ("a{sv}"));
    GVariant *reply = g_dbus_connection_call_sync (conn,
                                                    "org.freedesktop.Notifications",
                                                    "/org/freedesktop/Notifications",
                                                    "org.freedesktop.Notifications",
                                                    "Notify",
                                                    g_variant_new ("(susssasa{sv}i)",
                                                                   "OTPClient", (guint32)0,
                                                                   "com.github.paolostivanin.OTPClient",
                                                                   "OTP Token", body,
                                                                   &actions, &hints, (gint32)5000),
                                                    NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
    if (reply != NULL)
        g_variant_unref (reply);
    g_object_unref (conn);
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
    (void)conn; (void)sender; (void)path; (void)iface; (void)data;

    if (g_strcmp0 (method, "GetInitialResultSet") == 0 || g_strcmp0 (method, "GetSubsearchResultSet") == 0) {
        gchar **terms;
        if (g_strcmp0 (method, "GetInitialResultSet") == 0) {
            g_variant_get (params, "(^as)", &terms);
        } else {
            gchar **prev_results = NULL;
            g_variant_get (params, "(^as^as)", &prev_results, &terms);
            g_strfreev (prev_results);
        }
        GVariantBuilder builder;
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("as"));
        g_auto(GStrv) stripped = NULL;
        if (strip_keyword_or_skip (terms, &stripped)) {
            GPtrArray *entries = get_entries ();
            gsize stripped_len = g_strv_length (stripped);
            for (guint i = 0; i < entries->len; i++) {
                OtpSearchEntry *e = g_ptr_array_index (entries, i);
                if (entry_matches_terms (e, stripped, stripped_len))
                    g_variant_builder_add (&builder, "s", e->id);
            }
        }
        g_dbus_method_invocation_return_value (inv, g_variant_new ("(as)", &builder));
        g_strfreev (terms);
    } else if (g_strcmp0 (method, "GetResultMetas") == 0) {
        gchar **ids;
        g_variant_get (params, "(^as)", &ids);
        GVariantBuilder builder;
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("aa{sv}"));
        GPtrArray *entries = get_entries ();
        for (gsize j = 0; ids[j]; j++) {
            for (guint i = 0; i < entries->len; i++) {
                OtpSearchEntry *e = g_ptr_array_index (entries, i);
                if (g_strcmp0 (e->id, ids[j]) == 0) {
                    GVariantBuilder meta;
                    g_variant_builder_init (&meta, G_VARIANT_TYPE ("a{sv}"));
                    g_variant_builder_add (&meta, "{sv}", "id", g_variant_new_string (e->id));
                    g_variant_builder_add (&meta, "{sv}", "name", g_variant_new_string (e->label));
                    g_autofree gchar *desc = NULL;
                    if (e->db_name != NULL && e->db_name[0] != '\0')
                        desc = g_strdup_printf ("%s — %s", e->db_name,
                                                (e->issuer && e->issuer[0]) ? e->issuer : e->label);
                    else
                        desc = g_strdup (e->issuer ? e->issuer : "");
                    g_variant_builder_add (&meta, "{sv}", "description", g_variant_new_string (desc));
                    g_variant_builder_add (&meta, "{sv}", "icon", g_variant_new_string ("com.github.paolostivanin.OTPClient"));
                    g_variant_builder_add_value (&builder, g_variant_builder_end (&meta));
                }
            }
        }
        g_dbus_method_invocation_return_value (inv, g_variant_new ("(aa{sv})", &builder));
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
        const gchar *id;
        g_variant_get (params, "(&s^as u)", &id, NULL, NULL);
        g_autofree gchar *otp = NULL;
        g_autofree gchar *label = NULL;
        GPtrArray *entries = get_entries ();
        for (guint i = 0; i < entries->len; i++) {
            OtpSearchEntry *e = g_ptr_array_index (entries, i);
            if (g_strcmp0 (e->id, id) == 0) {
                otp = compute_otp_for_entry (e);
                label = g_strdup (e->label);
                break;
            }
        }
        copy_to_clipboard (otp, FALSE);
        send_notification (label, otp);
        g_dbus_method_invocation_return_value (inv, NULL);
    } else {
        g_dbus_method_invocation_return_value (inv, NULL);
    }
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
    (void)conn; (void)sender; (void)path; (void)iface; (void)data;

    if (g_strcmp0 (method, "Match") == 0) {
        const gchar *query;
        g_variant_get (params, "(&s)", &query);
        GVariantBuilder builder;
        g_variant_builder_init (&builder, G_VARIANT_TYPE ("a(sssida{sv})"));
        if (query && *query) {
            g_auto(GStrv) terms = g_strsplit_set (query, " \t", -1);
            g_auto(GStrv) stripped = NULL;
            if (strip_keyword_or_skip (terms, &stripped)) {
                gsize stripped_len = g_strv_length (stripped);
                GPtrArray *entries = get_entries ();
                for (guint i = 0; i < entries->len; i++) {
                    OtpSearchEntry *e = g_ptr_array_index (entries, i);
                    if (!entry_matches_terms (e, stripped, stripped_len)) continue;
                    GVariantBuilder props;
                    g_variant_builder_init (&props, G_VARIANT_TYPE ("a{sv}"));
                    // Deliberately do NOT include the OTP value in the subtitle:
                    // any process on the session bus can poll Match. The code is
                    // only handed out via Run, where the user sees a notification.
                    g_autofree gchar *sub = NULL;
                    if (e->db_name != NULL && e->db_name[0] != '\0')
                        sub = (e->issuer && *e->issuer)
                            ? g_strdup_printf ("%s — %s", e->db_name, e->issuer)
                            : g_strdup (e->db_name);
                    else
                        sub = g_strdup (e->issuer ? e->issuer : "");
                    g_variant_builder_add (&props, "{sv}", "subtext", g_variant_new_string (sub));
                    g_variant_builder_add (&props, "{sv}", "category", g_variant_new_string ("OTPClient"));
                    g_variant_builder_add (&builder, "(sssida{sv})",
                                           e->id, e->label,
                                           "com.github.paolostivanin.OTPClient",
                                           (gint32)0, (gdouble)1.0, &props);
                }
            }
        }
        GVariant *res = g_variant_builder_end (&builder);
        g_dbus_method_invocation_return_value (inv, g_variant_new_tuple (&res, 1));
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
        const gchar *id;
        g_variant_get (params, "(&s&s)", &id, NULL);
        g_autofree gchar *otp = NULL;
        g_autofree gchar *label = NULL;
        GPtrArray *entries = get_entries ();
        for (guint i = 0; i < entries->len; i++) {
            OtpSearchEntry *e = g_ptr_array_index (entries, i);
            if (g_strcmp0 (e->id, id) == 0) {
                otp = compute_otp_for_entry (e);
                label = g_strdup (e->label);
                break;
            }
        }
        copy_to_clipboard (otp, TRUE);
        send_notification (label, otp);
        g_dbus_method_invocation_return_value (inv, NULL);
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

    if (!gsettings_common_get_search_provider_enabled ())
        return 0;

    load_keyword_config ();

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

    if (set_memlock_value (&global_max_file_size) == MEMLOCK_ERR) {
        g_printerr ("Couldn't get the memlock value.\n");
        return 1;
    }
    gchar *init_msg = init_libs (global_max_file_size);
    if (init_msg != NULL) {
        g_printerr ("Error while initializing GCrypt: %s\n", init_msg);
        g_free (init_msg);
        return 1;
    }

    main_loop = g_main_loop_new (NULL, FALSE);
    if (force_kde)
        g_bus_own_name (G_BUS_TYPE_SESSION, KRUNNER_BUS, G_BUS_NAME_OWNER_FLAGS_NONE,
                        on_krunner_bus_acquired, NULL, on_name_lost, NULL, NULL);
    if (force_gnome)
        g_bus_own_name (G_BUS_TYPE_SESSION, GNOME_BUS, G_BUS_NAME_OWNER_FLAGS_NONE,
                        on_gnome_bus_acquired, NULL, on_name_lost, NULL, NULL);
    g_idle_add (prewarm_cache, NULL);
    g_main_loop_run (main_loop);
    clear_file_monitors ();
    if (cached_entries != NULL) {
        g_ptr_array_free (cached_entries, TRUE);
        cached_entries = NULL;
    }
    /* Wipe derived keys + per-sender state on shutdown. The kdf_cache entry
     * destroy callback explicit_bzero's the derived key before gcry_free. */
    kdf_cache_clear ();
    rate_buckets_clear ();
    g_clear_pointer (&g_keyword, g_free);
    g_clear_pointer (&g_keyword_fold, g_free);
    g_main_loop_unref (main_loop);
    return 0;
}
