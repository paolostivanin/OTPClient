#pragma once

#include <glib.h>
#include <jansson.h>
#include "common.h"

G_BEGIN_DECLS

#define OTP_HOTP_COUNTER_MAX ((guint64) (1ULL << 48))

/* Validation policy:
 * - Loaded databases are validated as a full root array before becoming live.
 * - Imported tokens are validated before entering a transaction.
 * - Direct generators (CLI/search provider) and GUI add/edit flows validate
 *   again at their local boundary so future bypasses fail closed.
 */

gboolean otp_secret_is_valid_base32 (const gchar *secret);

gboolean otp_validate_token_object  (json_t       *obj,
                                     gsize         index,
                                     GError      **err);

gboolean otp_validate_database_root (json_t       *root,
                                     GError      **err);

gboolean otp_validate_import_token  (const otp_t  *otp,
                                     GError      **err);

G_END_DECLS
