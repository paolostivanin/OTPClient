#include <glib.h>

#include "dbus-signatures.h"

/* A GVariant format string is never checked by the compiler, and getting one
 * wrong is close to invisible at runtime: g_variant_get validates the format
 * string against the value it was handed and, when that fails, logs a CRITICAL
 * and returns without writing a single out-parameter. Nothing in the tree makes
 * that level fatal, so the method silently does nothing.
 *
 * That is exactly what happened to ActivateResult, whose signature carried a
 * stray space inside the tuple: GNOME Shell listed the results correctly, and
 * pressing Enter copied nothing, notified nothing and reported nothing.
 *
 * g_variant_check_format_string is the public half of the same validation, so
 * run every signature the provider uses against the type its method actually
 * carries on the bus. A typo, a stray character or a signature that drifts away
 * from the introspection XML all come out as a failure here. */

typedef struct {
    const gchar *what;
    const gchar *format;
    /* The <arg type='...'/> list for that method in search-provider.c, in order
     * and wrapped in the tuple D-Bus puts them in. */
    const gchar *declared_type;
} Signature;

static const Signature signatures[] = {
    /* org.gnome.Shell.SearchProvider2 */
    { "GetInitialResultSet",   SP_SIG_GET_INITIAL_RESULT_SET,   "(as)"            },
    { "GetSubsearchResultSet", SP_SIG_GET_SUBSEARCH_RESULT_SET, "(asas)"          },
    { "GetResultMetas",        SP_SIG_GET_RESULT_METAS,         "(as)"            },
    { "ActivateResult",        SP_SIG_ACTIVATE_RESULT,          "(sasu)"          },
    { "result ids reply",      SP_SIG_REPLY_RESULT_IDS,         "(as)"            },
    { "result metas reply",    SP_SIG_REPLY_RESULT_METAS,       "(aa{sv})"        },
    /* org.kde.krunner1 */
    { "krunner Match",         SP_SIG_KRUNNER_MATCH,            "(s)"             },
    { "krunner Run",           SP_SIG_KRUNNER_RUN,              "(ss)"            },
    /* Outgoing calls */
    { "klipper setClipboard",  SP_SIG_KLIPPER_SET_CLIPBOARD,    "(s)"             },
    { "Notify",                SP_SIG_NOTIFY,                   "(susssasa{sv}i)" },
    { "AddNotification",       SP_SIG_ADD_NOTIFICATION,         "(sa{sv})"        },
    { "RemoveNotification",    SP_SIG_REMOVE_NOTIFICATION,      "(s)"             },
    { "Properties.Get",        SP_SIG_PROPERTIES_GET,           "(ss)"            },
    { "Properties.Get reply",  SP_SIG_REPLY_PROPERTIES_GET,     "(v)"             },
};

/* Deserialising an empty buffer gives the default value for any type, which is
 * all we need: g_variant_check_format_string looks at the type, not the data. */
static GVariant *
default_value_of_type (const gchar *type_string)
{
    g_autoptr (GBytes) empty = g_bytes_new (NULL, 0);
    GVariant *value = g_variant_new_from_bytes (G_VARIANT_TYPE (type_string), empty, FALSE);

    g_assert_nonnull (value);
    return g_variant_ref_sink (value);
}

static void
test_signatures_match_introspection (void)
{
    for (gsize i = 0; i < G_N_ELEMENTS (signatures); i++) {
        const Signature *sig = &signatures[i];
        g_autoptr (GVariant) value = default_value_of_type (sig->declared_type);

        if (!g_variant_check_format_string (value, sig->format, FALSE)) {
            g_error ("%s: format string '%s' does not parse a '%s', which is what the "
                     "introspection XML declares", sig->what, sig->format, sig->declared_type);
        }
    }
}

/* The check is only worth anything if it rejects the shape of the bug it is here
 * to catch. */
static void
test_a_stray_space_is_rejected (void)
{
    g_autoptr (GVariant) value = default_value_of_type ("(sasu)");

    g_assert_false (g_variant_check_format_string (value, "(&s^as u)", FALSE));
    g_assert_true (g_variant_check_format_string (value, SP_SIG_ACTIVATE_RESULT, FALSE));
}

/* And ActivateResult has to survive a real round-trip, not just a type check:
 * the terms have to come back out. An empty terms array is what made the broken
 * version look like a keyword mismatch rather than a parse failure. */
static void
test_activate_result_round_trip (void)
{
    const gchar *const sent_terms[] = { "otp", "alice", NULL };
    g_autoptr (GVariant) params = g_variant_ref_sink (
            g_variant_new ("(s^asu)", "otpclient:0:1", sent_terms, (guint32) 1234));

    const gchar *id = NULL;
    g_auto (GStrv) terms = NULL;
    g_variant_get (params, SP_SIG_ACTIVATE_RESULT, &id, &terms, NULL);

    g_assert_cmpstr (id, ==, "otpclient:0:1");
    g_assert_nonnull (terms);
    g_assert_cmpuint (g_strv_length (terms), ==, 2);
    g_assert_cmpstr (terms[0], ==, "otp");
    g_assert_cmpstr (terms[1], ==, "alice");
}

int
main (int    argc,
      char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_test_add_func ("/dbus-signatures/matches-introspection", test_signatures_match_introspection);
    g_test_add_func ("/dbus-signatures/stray-space-rejected", test_a_stray_space_is_rejected);
    g_test_add_func ("/dbus-signatures/activate-result-round-trip", test_activate_result_round_trip);
    return g_test_run ();
}
