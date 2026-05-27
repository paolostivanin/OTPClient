#pragma once

#include <libsecret/secret.h>

const SecretSchema *otpclient_get_schema (void) G_GNUC_CONST;

#define OTPCLIENT_SCHEMA  otpclient_get_schema ()

void on_password_stored  (GObject      *source,
                          GAsyncResult *result,
                          gpointer      unused);

void on_password_cleared (GObject      *source,
                          GAsyncResult *result,
                          gpointer      unused);

/* Synchronous round-trip test of the registered Secret Service provider:
 * store a probe value, look it up, verify it, then clear it. Uses a sentinel
 * attribute value so the probe cannot collide with a real db_path. Returns
 * TRUE iff every step succeeds. On failure, *error is set to the underlying
 * libsecret error (or a synthetic one for the verify-mismatch case). */
gboolean otpclient_secret_service_probe (GError **error);

/* Sync lookup of a database password, falling back to the v4 keyring
 * attribute if no entry exists under the v5 db_path-keyed attribute.
 * Covers users who upgraded across the pre-5.0 -> 5.0+ schema change
 * (issue #448). On success with the fallback path, *out_is_legacy is set
 * TRUE so callers can re-store under db_path and clear the legacy entry.
 * Pass NULL for out_is_legacy if you don't care.
 *
 * Returns the password (free with secret_password_free) or NULL. *err is
 * set on libsecret failures; NULL with *err==NULL means "no entry stored". */
gchar *otpclient_secret_lookup_with_legacy_fallback (const gchar  *db_path,
                                                     gboolean     *out_is_legacy,
                                                     GError      **err);

/* Sync lookup of the v4 legacy keyring entry only. Used by the GUI when
 * the async db_path-keyed lookup has already returned NULL and we only
 * need to check the fallback. Returns NULL if not found. */
gchar *otpclient_secret_lookup_legacy_only (GError **err);

/* TRUE iff a v4 legacy keyring entry exists. Used by the GUI startup
 * migration probe (issue #448) to decide whether to auto-enable Secret
 * Service for users upgrading from pre-5.0. Errors are swallowed (treated
 * as "no entry") because the broken-keyring case is handled separately. */
gboolean otpclient_secret_legacy_entry_exists (void);

/* Async clear of the v4 legacy keyring entry. Internal cleanup: failures
 * are logged but no user-facing notification fires (a stale legacy entry
 * is self-healing on the next successful unlock). */
void otpclient_secret_clear_legacy_async (void);

/* Sync clear of the v4 legacy keyring entry. CLI helper; logs a warning
 * on failure but does not return an error -- a stale legacy entry is not
 * worth aborting a successful CLI invocation over. */
void otpclient_secret_clear_legacy_sync (void);