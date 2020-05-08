#include <glib.h>
#include <gcrypt.h>
#include <jansson.h>
#include <time.h>
#include "file-size.h"
#include "parse-uri.h"


GSList *
get_freeotpplus_data (const gchar     *path,
                      GError         **err)
{
    GSList *otps = NULL;
    gchar *sec_buf = gcry_calloc_secure (get_file_size (path), 1);
    if (!g_file_get_contents (path, &sec_buf, NULL, err)) {
        g_printerr("Couldn't read into memory the freeotp txt file\n");
        return NULL;
    }

    set_otps_from_uris (sec_buf, &otps);

    gcry_free (sec_buf);

    return otps;
}


gchar *
export_freeotpplus (const gchar *export_path,
                    json_t      *json_db_data)
{
    gchar *uri, *constructed_label;
    json_t *db_obj;
    gsize index;

    FILE *fp = fopen (export_path, "w");
    if (fp == NULL) {
        return g_strdup ("couldn't create the file object");
    }

    json_array_foreach (json_db_data, index, db_obj) {
        uri = g_strconcat ("otpauth://", NULL);
        const gchar *issuer = json_string_value (json_object_get (db_obj, "issuer"));
        if (issuer != NULL && g_ascii_strcasecmp (issuer, "steam") == 0) {
            uri = g_strconcat (uri, "totp/", NULL);
            constructed_label = g_strconcat ("Steam:", json_string_value (json_object_get (db_obj, "label")), NULL);
        } else {
            uri = g_strconcat (uri, g_utf8_strdown (json_string_value (json_object_get (db_obj, "type")),  -1), "/", NULL);
            if (issuer != NULL && g_utf8_strlen (issuer, -1) > 0) {
                constructed_label = g_strconcat (json_string_value (json_object_get (db_obj, "issuer")),
                                                 ":",
                                                 json_string_value (json_object_get (db_obj, "label")),
                                                 NULL);
            } else {
                constructed_label = g_strdup (json_string_value (json_object_get (db_obj, "label")));
            }
        }

        gchar *escaped_label = g_uri_escape_string (constructed_label, NULL, FALSE);
        uri = g_strconcat (uri, escaped_label, NULL);
        uri = g_strconcat (uri, "?secret=", json_string_value (json_object_get (db_obj, "secret")), NULL);
        if (issuer != NULL && g_ascii_strcasecmp (issuer, "steam") == 0) {
            uri = g_strconcat (uri, "&issuer=Steam", NULL);
        }
        if (issuer != NULL && g_utf8_strlen (issuer, -1) > 0) {
            uri = g_strconcat (uri, "&issuer=", json_string_value (json_object_get (db_obj, "issuer")), NULL);
        }

        uri = g_strconcat (uri, "&digits=", g_strdup_printf ("%lld", json_integer_value ( json_object_get (db_obj, "digits"))), NULL);
        uri = g_strconcat (uri, "&algorithm=", json_string_value ( json_object_get (db_obj, "algo")), NULL);

        if (g_ascii_strcasecmp (json_string_value (json_object_get (db_obj, "type")), "TOTP") == 0) {
            uri = g_strconcat (uri, "&period=", g_strdup_printf ("%lld",json_integer_value ( json_object_get (db_obj, "period"))), NULL);
        } else {
            uri = g_strconcat (uri, "&counter=", g_strdup_printf ("%lld",json_integer_value ( json_object_get (db_obj, "counter"))), NULL);
        }

        uri = g_strconcat (uri, "\n", NULL);
        fwrite (uri, strlen (uri), 1, fp);

        g_free (constructed_label);
        g_free (escaped_label);
        g_free (uri);
    }

    fclose (fp);

    return NULL;
}
