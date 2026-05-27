#include <libsecret/secret.h>
#include <gio/gio.h>
#include <string.h>
#include "secret-schema.h"

/* Sentinel value used as the "string" attribute for the keyring probe.
 * Real db_paths are absolute filesystem paths, so they cannot match this. */
#define OTPCLIENT_SECRET_PROBE_ATTR "__otpclient_probe__"

/* Attribute value used by pre-5.0 builds for the (single) database password.
 * v5 switched to keying by absolute db_path so multi-database setups can
 * each have their own keyring entry; the legacy value is only consulted on
 * the fallback path to migrate upgraders without forcing them to retype the
 * password on first launch (issue #448). */
#define OTPCLIENT_SECRET_LEGACY_ATTR "main_pwd"

const SecretSchema *
otpclient_get_schema (void)
{
    static const SecretSchema the_schema = {
            "com.github.paolostivanin.OTPClient", SECRET_SCHEMA_NONE,
            {
                    {  "string", SECRET_SCHEMA_ATTRIBUTE_STRING },
                    {  "NULL", 0 },
            },
            0,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
            NULL,
    };
    return &the_schema;
}


/* H6: surface secret-service failures rather than swallowing them with a
 * silent g_printerr. If the caller passed a GApplication as user_data, also
 * fire a desktop notification so a user with no terminal sees the failure
 * (otherwise their next launch would prompt for the password and they'd have
 * no idea why the keyring wasn't used). The CLI passes NULL — there's no
 * application to notify, but g_warning still logs to the journal. */
static void
notify_secret_failure (gpointer    user_data,
                       const gchar *title,
                       const gchar *body)
{
    g_warning ("%s: %s", title, body);
    if (user_data == NULL || !G_IS_APPLICATION (user_data))
        return;
    GNotification *n = g_notification_new (title);
    g_notification_set_body (n, body);
    g_application_send_notification (G_APPLICATION (user_data), "secret-service-failure", n);
    g_object_unref (n);
}


void
on_password_stored (GObject *source         __attribute__((unused)),
                    GAsyncResult *result,
                    gpointer user_data)
{
    GError *error = NULL;
    secret_password_store_finish (result, &error);
    if (error != NULL) {
        notify_secret_failure (user_data,
                               "Couldn't store the password in the secret service",
                               error->message);
        g_error_free (error);
    }
}


void
on_password_cleared (GObject *source         __attribute__((unused)),
                     GAsyncResult *result,
                     gpointer user_data)
{
    GError *error = NULL;
    gboolean removed = secret_password_clear_finish (result, &error);
    if (error != NULL) {
        notify_secret_failure (user_data,
                               "Couldn't remove the password from the secret service",
                               error->message);
        g_error_free (error);
    }
    if (removed == TRUE) {
        g_message ("Password successfully removed from the secret service.");
    }
}


gboolean
otpclient_secret_service_probe (GError **error)
{
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

    /* Fresh random payload each call: a stale probe value left behind by a
     * previously crashed run cannot produce a false positive — the verify
     * step compares against this run's uuid. */
    g_autofree gchar *expected = g_uuid_string_random ();

    GError *probe_err = NULL;
    gchar  *retrieved  = NULL;
    gboolean ok = FALSE;

    if (!secret_password_store_sync (OTPCLIENT_SCHEMA,
                                     SECRET_COLLECTION_DEFAULT,
                                     "OTPClient keyring probe",
                                     expected,
                                     NULL,
                                     &probe_err,
                                     "string", OTPCLIENT_SECRET_PROBE_ATTR,
                                     NULL))
    {
        goto cleanup;
    }

    retrieved = secret_password_lookup_sync (OTPCLIENT_SCHEMA, NULL, &probe_err,
                                             "string", OTPCLIENT_SECRET_PROBE_ATTR,
                                             NULL);
    if (probe_err != NULL)
        goto cleanup;

    if (retrieved == NULL || strcmp (retrieved, expected) != 0)
    {
        g_set_error_literal (&probe_err, G_IO_ERROR, G_IO_ERROR_FAILED,
                             "keyring round-trip returned different data than was written");
        goto cleanup;
    }

    ok = TRUE;

cleanup:
    /* Always attempt cleanup; ignore errors so we don't mask the original
     * failure (or, on success, leave a stray probe value behind). */
    secret_password_clear_sync (OTPCLIENT_SCHEMA, NULL, NULL,
                                "string", OTPCLIENT_SECRET_PROBE_ATTR,
                                NULL);
    if (retrieved != NULL)
        secret_password_free (retrieved);

    if (probe_err != NULL)
        g_propagate_error (error, probe_err);

    return ok;
}


gchar *
otpclient_secret_lookup_with_legacy_fallback (const gchar  *db_path,
                                              gboolean     *out_is_legacy,
                                              GError      **err)
{
    g_return_val_if_fail (db_path != NULL, NULL);
    g_return_val_if_fail (err == NULL || *err == NULL, NULL);

    if (out_is_legacy != NULL)
        *out_is_legacy = FALSE;

    GError *local_err = NULL;
    gchar *pwd = secret_password_lookup_sync (OTPCLIENT_SCHEMA, NULL, &local_err,
                                              "string", db_path, NULL);
    if (local_err != NULL) {
        g_propagate_error (err, local_err);
        return NULL;
    }
    if (pwd != NULL)
        return pwd;

    pwd = secret_password_lookup_sync (OTPCLIENT_SCHEMA, NULL, &local_err,
                                       "string", OTPCLIENT_SECRET_LEGACY_ATTR, NULL);
    if (local_err != NULL) {
        g_propagate_error (err, local_err);
        return NULL;
    }
    if (pwd != NULL && out_is_legacy != NULL)
        *out_is_legacy = TRUE;
    return pwd;
}


gchar *
otpclient_secret_lookup_legacy_only (GError **err)
{
    g_return_val_if_fail (err == NULL || *err == NULL, NULL);
    return secret_password_lookup_sync (OTPCLIENT_SCHEMA, NULL, err,
                                        "string", OTPCLIENT_SECRET_LEGACY_ATTR,
                                        NULL);
}


gboolean
otpclient_secret_legacy_entry_exists (void)
{
    GError *err = NULL;
    gchar *pwd = secret_password_lookup_sync (OTPCLIENT_SCHEMA, NULL, &err,
                                              "string", OTPCLIENT_SECRET_LEGACY_ATTR,
                                              NULL);
    if (err != NULL) {
        g_clear_error (&err);
        return FALSE;
    }
    if (pwd != NULL) {
        secret_password_free (pwd);
        return TRUE;
    }
    return FALSE;
}


void
otpclient_secret_clear_legacy_async (void)
{
    /* Pass NULL as user_data: the migration cleanup is internal, not a
     * user-initiated action. on_password_cleared's notification short-circuits
     * on a NULL/non-GApplication user_data, so a clear failure logs a warning
     * but does not pop up a confusing "Couldn't remove the password" toast.
     * A stale legacy entry is self-healing on the next successful unlock. */
    secret_password_clear (OTPCLIENT_SCHEMA, NULL,
                           on_password_cleared, NULL,
                           "string", OTPCLIENT_SECRET_LEGACY_ATTR,
                           NULL);
}


void
otpclient_secret_clear_legacy_sync (void)
{
    GError *err = NULL;
    secret_password_clear_sync (OTPCLIENT_SCHEMA, NULL, &err,
                                "string", OTPCLIENT_SECRET_LEGACY_ATTR,
                                NULL);
    if (err != NULL) {
        g_warning ("Couldn't clear v4 secret-service entry: %s", err->message);
        g_clear_error (&err);
    }
}