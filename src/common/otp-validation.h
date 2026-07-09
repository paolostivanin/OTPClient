#pragma once

#include <glib.h>
#include <jansson.h>
#include "common.h"

G_BEGIN_DECLS

/* Exclusive upper bound: valid HOTP counters satisfy counter < 2^48. */
#define OTP_HOTP_COUNTER_MAX ((guint64) (1ULL << 48))

/* Validation policy:
 * - Loaded databases are validated as a full root array before becoming live.
 * - Imported tokens are validated before entering a transaction.
 * - Direct generators (CLI/search provider) and GUI add/edit flows validate
 *   again at their local boundary so future bypasses fail closed.
 *
 * Repair policy (issue #462):
 * - A token with neither a label nor an issuer is anonymous. Rejecting one
 *   aborts the whole load, locking the user out of every other token. On the
 *   non-interactive load and file/URI import paths we instead synthesize a
 *   placeholder label so the token stays usable and editable. Interactive
 *   entry (manual-add, edit) keeps rejecting: the user is right there to fix it.
 */

gboolean otp_secret_is_valid_base32 (const gchar *secret);

gboolean otp_validate_token_object  (json_t       *obj,
                                     gsize         index,
                                     GError      **err);

gboolean otp_validate_database_root (json_t       *root,
                                     GError      **err);

gboolean otp_validate_import_token  (const otp_t  *otp,
                                     GError      **err);

/* Give an anonymous token (neither label nor issuer) a synthesized "Unknown N"
 * label. Idempotent: a token that already has a label or issuer is left as-is.
 * Returns TRUE when a placeholder was assigned. */
gboolean otp_repair_anonymous_token_object (json_t      *obj,
                                            gsize        index);

/* Repair every anonymous token in a database root array in place. Returns the
 * number of tokens that received a placeholder. */
guint    otp_repair_database_root          (json_t      *root);

/* Same as otp_repair_anonymous_token_object for an in-flight import token,
 * writing the placeholder into otp->account_name. */
gboolean otp_repair_anonymous_import_token (otp_t       *otp,
                                            gsize        index);

G_END_DECLS
