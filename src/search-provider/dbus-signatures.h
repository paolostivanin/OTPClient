#pragma once

#include <glib.h>

G_BEGIN_DECLS

/* Every GVariant format string the provider hands to g_variant_get or
 * g_variant_new, in one place.
 *
 * They live here because a malformed one is invisible: g_variant_get validates
 * the format string against the value and, when that fails, logs a CRITICAL and
 * returns without writing a single out-parameter. The compiler cannot catch it,
 * and nothing in the tree makes that log level fatal, so the symptom is a
 * method that silently does nothing. A stray space in the ActivateResult
 * signature meant GNOME Shell result activation never worked at all.
 *
 * test_dbus_signatures.c checks every constant below against the type its
 * method declares in the introspection XML, so a typo added here fails the
 * build's test run instead of shipping.
 */

/* org.gnome.Shell.SearchProvider2 */
#define SP_SIG_GET_INITIAL_RESULT_SET    "(^as)"
#define SP_SIG_GET_SUBSEARCH_RESULT_SET  "(^as^as)"
#define SP_SIG_GET_RESULT_METAS          "(^as)"
#define SP_SIG_ACTIVATE_RESULT           "(&s^asu)"
#define SP_SIG_REPLY_RESULT_IDS          "(as)"
#define SP_SIG_REPLY_RESULT_METAS        "(aa{sv})"

/* org.kde.krunner1 */
#define SP_SIG_KRUNNER_MATCH             "(&s)"
#define SP_SIG_KRUNNER_RUN               "(&s&s)"

/* Outgoing calls */
#define SP_SIG_KLIPPER_SET_CLIPBOARD     "(s)"
#define SP_SIG_NOTIFY                    "(susssasa{sv}i)"

/* org.freedesktop.portal.Notification, and the property read that tells us
 * which version of it the host runs */
#define SP_SIG_ADD_NOTIFICATION          "(sa{sv})"
#define SP_SIG_REMOVE_NOTIFICATION       "(s)"
#define SP_SIG_PROPERTIES_GET            "(ss)"
#define SP_SIG_REPLY_PROPERTIES_GET      "(v)"

G_END_DECLS
