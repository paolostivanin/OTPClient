#include <glib.h>
#include "imports.h"
#include "common/common.h"
#include "gui-common.h"


static void parse_uri           (const gchar   *uri,
                                 GSList       **otps);

static void parse_parameters    (const gchar   *modified_uri,
                                 otp_t         *otp);


void
set_otps_from_uris (const gchar   *otpauth_uris,
                    GSList       **otps)
{
    gchar **uris = g_strsplit (otpauth_uris, "\n", -1);
    guint i = 0, uris_len = g_strv_length (uris);
    gchar *haystack = NULL;
    if (uris_len > 0) {
        for (; i < uris_len; i++) {
            haystack = g_strrstr (uris[i], "otpauth");
            if (haystack != NULL) {
                parse_uri (haystack, otps);
            }
        }
    }
    g_strfreev (uris);
}


gchar *
get_otpauth_uri (AppData *app_data,
                 json_t  *obj)
{
    gchar *constructed_label = NULL;
    json_t *db_obj = NULL;

    if (app_data == NULL) {
        db_obj = obj;
    } else {
        GtkTreeModel *model = gtk_tree_view_get_model (app_data->tree_view);
        GtkListStore *list_store = GTK_LIST_STORE(model);
        GtkTreeIter iter;

        if (gtk_tree_selection_get_selected (gtk_tree_view_get_selection (app_data->tree_view), &model, &iter) == FALSE) {
            return NULL;
        }

        guint row_number = get_row_number_from_iter (list_store, iter);
        db_obj = json_array_get (app_data->db_data->json_data, row_number);
    }

    GString *uri = g_string_new (NULL);
    g_string_append (uri, "otpauth://");
    const gchar *issuer = json_string_value (json_object_get (db_obj, "issuer"));
    if (issuer != NULL && g_ascii_strcasecmp (issuer, "steam") == 0) {
        g_string_append (uri, "totp/");
        constructed_label = g_strconcat ("Steam:", json_string_value (json_object_get (db_obj, "label")), NULL);
    } else {
        g_string_append (uri, g_utf8_strdown (json_string_value (json_object_get (db_obj, "type")),  -1));
        g_string_append (uri, "/");
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
    g_string_append (uri, escaped_label);
    g_string_append (uri, "?secret=");
    g_string_append (uri, json_string_value (json_object_get (db_obj, "secret")));
    if (issuer != NULL && g_ascii_strcasecmp (issuer, "steam") == 0) {
        g_string_append (uri, "&issuer=Steam");
    }
    if (issuer != NULL && g_utf8_strlen (issuer, -1) > 0) {
        g_string_append (uri, "&issuer=");
        g_string_append (uri, json_string_value (json_object_get (db_obj, "issuer")));
    }

    gchar *str_to_append = NULL;
    g_string_append (uri, "&digits=");
    str_to_append = g_strdup_printf ("%lld", json_integer_value ( json_object_get (db_obj, "digits")));
    g_string_append (uri,str_to_append);
    g_free (str_to_append);
    g_string_append (uri, "&algorithm=");
    g_string_append (uri, json_string_value ( json_object_get (db_obj, "algo")));

    if (g_ascii_strcasecmp (json_string_value (json_object_get (db_obj, "type")), "TOTP") == 0) {
        g_string_append (uri, "&period=");
        str_to_append = g_strdup_printf ("%lld", json_integer_value ( json_object_get (db_obj, "period")));
        g_string_append (uri, str_to_append);
        g_free (str_to_append);
    } else {
        g_string_append (uri, "&counter=");
        str_to_append = g_strdup_printf ("%lld", json_integer_value ( json_object_get (db_obj, "counter")));
        g_string_append (uri, str_to_append);
        g_free (str_to_append);
    }

    g_string_append (uri, "\n");

    g_free (constructed_label);
    g_free (escaped_label);

    return g_string_free (uri, FALSE);
}


static void
parse_uri (const gchar   *uri,
           GSList       **otps)
{
    const gchar *uri_copy = uri;
    if (g_ascii_strncasecmp (uri_copy, "otpauth://", 10) != 0) {
        return;
    }
    uri_copy += 10;

    otp_t *otp = g_new0 (otp_t, 1);
    // set default digits value to 6. If something else is specified, it will be read later on
    otp->digits = 6;
    if (g_ascii_strncasecmp (uri_copy, "totp/", 5) == 0) {
        otp->type = g_strdup ("TOTP");
        otp->period = 30;
    } else if (g_ascii_strncasecmp (uri_copy, "hotp/", 5) == 0) {
        otp->type = g_strdup ("HOTP");
    } else {
        g_free (otp);
        return;
    }
    uri_copy += 5;

    if (g_strrstr (uri_copy, "algorithm") == NULL) {
        // if the uri doesn't contain the algo parameter, fallback to sha1
        otp->algo = g_strdup ("SHA1");
    }
    parse_parameters (uri_copy, otp);

    *otps = g_slist_append (*otps, g_memdupX (otp, sizeof (otp_t)));
    g_free (otp);
}


static void
parse_parameters (const gchar   *modified_uri,
                  otp_t         *otp)
{
    gchar **tokens = g_strsplit (modified_uri, "?", -1);
    gchar *escaped_issuer_and_label = g_uri_unescape_string (tokens[0], NULL);
    gchar *mod_uri_copy_utf8 = g_utf8_offset_to_pointer (modified_uri, g_utf8_strlen (tokens[0], -1) + 1);
    g_strfreev (tokens);

    tokens = g_strsplit (escaped_issuer_and_label, ":", -1);
    if (tokens[0] && tokens[1]) {
        otp->issuer = g_strdup (g_strstrip (tokens[0]));
        otp->account_name = g_strdup (g_strstrip (tokens[1]));

    } else {
        otp->account_name = g_strdup (g_strstrip (tokens[0]));
    }
    g_free (escaped_issuer_and_label);
    g_strfreev (tokens);

    tokens = g_strsplit (mod_uri_copy_utf8, "&", -1);
    gint i = 0;
    while (tokens[i]) {
        if (g_ascii_strncasecmp (tokens[i], "secret=", 7) == 0) {
            tokens[i] += 7;
            otp->secret = secure_strdup (tokens[i]);
            tokens[i] -= 7;
        } else if (g_ascii_strncasecmp (tokens[i], "algorithm=", 10) == 0) {
            tokens[i] += 10;
            if (g_ascii_strcasecmp (tokens[i], "SHA1") == 0 ||
                g_ascii_strcasecmp (tokens[i], "SHA256") == 0 ||
                g_ascii_strcasecmp (tokens[i], "SHA512") == 0) {
                otp->algo = g_ascii_strup (tokens[i], -1);
            }
            tokens[i] -= 10;
        } else if (g_ascii_strncasecmp (tokens[i], "period=", 7) == 0) {
            tokens[i] += 7;
            otp->period = (guint8) g_ascii_strtoll (tokens[i], NULL, 10);
            tokens[i] -= 7;
        } else if (g_ascii_strncasecmp (tokens[i], "digits=", 7) == 0) {
            tokens[i] += 7;
            otp->digits = (guint8) g_ascii_strtoll (tokens[i], NULL, 10);
            tokens[i] -= 7;
        } else if (g_ascii_strncasecmp (tokens[i], "issuer=", 7) == 0) {
            tokens[i] += 7;
            if (!otp->issuer) {
                otp->issuer = g_strdup (g_strstrip (tokens[i]));
            }
            tokens[i] -= 7;
        } else if (g_ascii_strncasecmp (tokens[i], "counter=", 8) == 0) {
            tokens[i] += 8;
            otp->counter = (guint64)g_ascii_strtoll (tokens[i], NULL, 10);
            tokens[i] -= 8;
        }
        i++;
    }
    g_strfreev (tokens);
}
