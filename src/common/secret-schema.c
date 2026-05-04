#include <libsecret/secret.h>
#include <gio/gio.h>

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