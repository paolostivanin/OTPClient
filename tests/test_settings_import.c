/* Three of the exportable keys, minimize-to-tray, start-minimized and
 * autostart, are not settings the app can honour by writing them. What they
 * stand for is a login-time entry and a background permission, both owned by
 * the desktop, and an import can only ask for those later. So the import has to
 * report that it wrote one, and leave a durable note for whoever is in a
 * position to act on it: the CLI cannot talk to the portal at all, and the GUI
 * can be killed between the write and the portal answering.
 *
 * The note is the interesting part. Without it, a Flatpak import that turns
 * everything off leaves nothing behind that a later launch could notice, since
 * the sandbox cannot see the entry the keys used to stand for, and the stale
 * login-time launch survives forever.
 *
 * The reconciliation that consumes all this is portal work and is not reachable
 * from here. What is pinned down is the contract between the two halves. */

#include <glib.h>
#include <gio/gio.h>
#include <jansson.h>
#include "settings-import-export.h"
#include "gsettings-common.h"

#define PENDING_KEY "startup-reconcile-pending"

static GSettings *
settings_or_skip (void)
{
    GSettings *settings = gsettings_common_get_settings ();
    if (settings == NULL)
        g_test_skip ("the GSettings schema is not available");
    return settings;
}

/* Every test starts from a known place, since the memory backend is per
 * process and one import would otherwise be visible to the next. */
static GSettings *
fixture_settings (void)
{
    GSettings *settings = settings_or_skip ();
    if (settings == NULL)
        return NULL;

    g_settings_set_boolean (settings, PENDING_KEY, FALSE);
    g_settings_set_boolean (settings, "autostart", FALSE);
    g_settings_set_boolean (settings, "minimize-to-tray", FALSE);
    g_settings_set_boolean (settings, "start-minimized", FALSE);
    g_settings_set_boolean (settings, "hide-otps", TRUE);
    return settings;
}

static gboolean
import (const gchar *json, gboolean *touched_startup)
{
    g_autoptr (GError) err = NULL;
    gboolean ok = import_settings_from_json (json, touched_startup, &err);
    g_assert_no_error (err);
    return ok;
}

/* An import with nothing to say about starting up must not cost a portal round
 * trip, nor leave a note asking for one. */
static void
test_ordinary_import_is_not_startup (void)
{
    g_autoptr (GSettings) settings = fixture_settings ();
    if (settings == NULL)
        return;

    gboolean touched = TRUE;   /* poisoned: the callee has to clear it */
    g_assert_true (import ("{\"hide-otps\": false}", &touched));

    g_assert_false (touched);
    g_assert_false (g_settings_get_boolean (settings, PENDING_KEY));
    g_assert_false (g_settings_get_boolean (settings, "hide-otps"));
}

/* The case the durable note exists for. Both values are false, so nothing in
 * the imported state hints that anything needs doing, and under Flatpak the
 * entry they used to stand for cannot be seen either. */
static void
test_startup_import_turning_everything_off (void)
{
    g_autoptr (GSettings) settings = fixture_settings ();
    if (settings == NULL)
        return;

    g_settings_set_boolean (settings, "autostart", TRUE);

    gboolean touched = FALSE;
    g_assert_true (import ("{\"autostart\": false, \"minimize-to-tray\": false}", &touched));

    g_assert_true (touched);
    g_assert_true (g_settings_get_boolean (settings, PENDING_KEY));
    g_assert_false (g_settings_get_boolean (settings, "autostart"));
}

static void
test_startup_import_turning_something_on (void)
{
    g_autoptr (GSettings) settings = fixture_settings ();
    if (settings == NULL)
        return;

    gboolean touched = FALSE;
    g_assert_true (import ("{\"minimize-to-tray\": true}", &touched));

    g_assert_true (touched);
    g_assert_true (g_settings_get_boolean (settings, PENDING_KEY));
    g_assert_true (g_settings_get_boolean (settings, "minimize-to-tray"));
}

/* start-minimized counts too: it is baked into the entry's argv, so changing it
 * means rewriting the entry. */
static void
test_start_minimized_counts_as_startup (void)
{
    g_autoptr (GSettings) settings = fixture_settings ();
    if (settings == NULL)
        return;

    gboolean touched = FALSE;
    g_assert_true (import ("{\"start-minimized\": true}", &touched));

    g_assert_true (touched);
    g_assert_true (g_settings_get_boolean (settings, PENDING_KEY));
}

/* A value of the wrong type is skipped, so the key was not written and there is
 * nothing to reconcile. Reporting it anyway would mean a portal round trip, and
 * under Flatpak a permission entry, on behalf of a value that was thrown away. */
static void
test_malformed_startup_value_is_not_reported (void)
{
    g_autoptr (GSettings) settings = fixture_settings ();
    if (settings == NULL)
        return;

    gboolean touched = FALSE;
    g_assert_true (import ("{\"autostart\": \"yes\"}", &touched));

    g_assert_false (touched);
    g_assert_false (g_settings_get_boolean (settings, PENDING_KEY));
    g_assert_false (g_settings_get_boolean (settings, "autostart"));
}

/* One good startup key alongside a malformed one still has to be reported: the
 * good one was written and the desktop has not been told. */
static void
test_one_good_startup_key_is_enough (void)
{
    g_autoptr (GSettings) settings = fixture_settings ();
    if (settings == NULL)
        return;

    gboolean touched = FALSE;
    g_assert_true (import ("{\"autostart\": \"yes\", \"minimize-to-tray\": true}", &touched));

    g_assert_true (touched);
    g_assert_true (g_settings_get_boolean (settings, PENDING_KEY));
}

/* The flag is a record of this installation, not a preference, so exporting and
 * re-importing must not carry it between machines or resurrect it. */
static void
test_pending_flag_is_not_exportable (void)
{
    g_autoptr (GSettings) settings = fixture_settings ();
    if (settings == NULL)
        return;

    g_settings_set_boolean (settings, PENDING_KEY, TRUE);

    g_autoptr (GError) err = NULL;
    g_autofree gchar *json = export_settings_to_json (&err);
    g_assert_no_error (err);
    g_assert_nonnull (json);
    g_assert_null (strstr (json, PENDING_KEY));

    /* And an import carrying it is ignored rather than obeyed. */
    g_settings_set_boolean (settings, PENDING_KEY, FALSE);
    gboolean touched = FALSE;
    g_assert_true (import ("{\"" PENDING_KEY "\": true}", &touched));
    g_assert_false (touched);
    g_assert_false (g_settings_get_boolean (settings, PENDING_KEY));
}

/* Which keys belong in a backup is a decision, and the only way to keep making
 * it is to notice when a new one turns up. Everything in the schema is
 * exportable unless it is named here, with the reason, so adding a preference
 * and forgetting the backup fails rather than passing quietly. That is not
 * hypothetical: search-provider-keyword and clipboard-clear-timeout were both
 * settings the user picks in the dialog and neither survived an export. */
static const gchar *const not_exportable[] = {
    /* Machine-local file locations, not preferences. Restoring these on another
     * machine points the app at databases that are not there. */
    "db-path",
    "db-list",
    /* Per-display geometry. */
    "window-width",
    "window-height",
    /* One-time migration records and internal bookkeeping, meaningless to
     * anyone else's install. */
    "secret-service-v4-migrated",
    "startup-reconcile-pending",
    "last-seen-version",
    /* Backup nagging state, about this machine's backups rather than about how
     * the user wants the app to behave. */
    "last-export-time",
    "backup-banner-snoozed-until",
    NULL
};

static void
test_every_preference_is_backed_up (void)
{
    g_autoptr (GSettingsSchema) schema = NULL;
    GSettingsSchemaSource *source = g_settings_schema_source_get_default ();
    if (source != NULL)
        schema = g_settings_schema_source_lookup (source, "com.github.paolostivanin.OTPClient", TRUE);
    if (schema == NULL) {
        g_test_skip ("the GSettings schema is not available");
        return;
    }

    g_autoptr (GError) err = NULL;
    g_autofree gchar *json = export_settings_to_json (&err);
    g_assert_no_error (err);
    g_assert_nonnull (json);

    json_error_t jerr;
    json_t *root = json_loads (json, 0, &jerr);
    g_assert_nonnull (root);

    g_auto (GStrv) keys = g_settings_schema_list_keys (schema);
    for (gsize i = 0; keys[i] != NULL; i++) {
        if (g_strv_contains (not_exportable, keys[i]))
            g_assert_null (json_object_get (root, keys[i]));
        else if (json_object_get (root, keys[i]) == NULL)
            g_error ("schema key '%s' is neither exported nor listed as "
                     "deliberately excluded", keys[i]);
    }

    /* And nothing is exported that the schema does not have, which would import
     * as a silently ignored key on the way back in. */
    const gchar *name;
    json_t *value;
    json_object_foreach (root, name, value) {
        (void) value;
        if (!g_settings_schema_has_key (schema, name))
            g_error ("exported key '%s' is not in the schema", name);
    }

    json_decref (root);
}

/* The out parameter is optional; the durable note is not. */
static void
test_null_out_parameter_still_persists (void)
{
    g_autoptr (GSettings) settings = fixture_settings ();
    if (settings == NULL)
        return;

    g_assert_true (import ("{\"autostart\": true}", NULL));
    g_assert_true (g_settings_get_boolean (settings, PENDING_KEY));
}

int
main (int argc, char *argv[])
{
    /* These tests write real keys. The memory backend keeps that inside the
     * process, and CMake points GSETTINGS_SCHEMA_DIR at the schema compiled
     * from this checkout rather than at whatever is installed. */
    g_setenv ("GSETTINGS_BACKEND", "memory", TRUE);

    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/settings-import/ordinary-key-is-not-startup",
                     test_ordinary_import_is_not_startup);
    g_test_add_func ("/settings-import/startup/everything-off",
                     test_startup_import_turning_everything_off);
    g_test_add_func ("/settings-import/startup/something-on",
                     test_startup_import_turning_something_on);
    g_test_add_func ("/settings-import/startup/start-minimized-counts",
                     test_start_minimized_counts_as_startup);
    g_test_add_func ("/settings-import/startup/malformed-is-not-reported",
                     test_malformed_startup_value_is_not_reported);
    g_test_add_func ("/settings-import/startup/one-good-key-is-enough",
                     test_one_good_startup_key_is_enough);
    g_test_add_func ("/settings-import/pending-flag-is-not-exportable",
                     test_pending_flag_is_not_exportable);
    g_test_add_func ("/settings-import/every-preference-is-backed-up",
                     test_every_preference_is_backed_up);
    g_test_add_func ("/settings-import/null-out-parameter-still-persists",
                     test_null_out_parameter_still_persists);

    return g_test_run ();
}
