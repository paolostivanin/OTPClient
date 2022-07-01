#include <libsecret/secret.h>

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


void
on_password_stored (GObject *source         __attribute__((unused)),
                    GAsyncResult *result,
                    gpointer unused         __attribute__((unused)))
{
    GError *error = NULL;
    secret_password_store_finish (result, &error);
    if (error != NULL) {
        g_printerr ("Couldn't store the password in the secret service.\n"
                    "The error was: %s", error->message);
        g_error_free (error);
    }
}


void
on_password_cleared (GObject *source         __attribute__((unused)),
                     GAsyncResult *result,
                     gpointer unused         __attribute__((unused)))
{
    GError *error = NULL;
    gboolean removed = secret_password_clear_finish (result, &error);
    if (error != NULL) {
        g_printerr ("Couldn't remove the password in the secret service.\n"
                    "The error was: %s", error->message);
        g_error_free (error);
    }
    if (removed == TRUE) {
        g_print ("Password successfully removed from the secret service.\n");
    }
}