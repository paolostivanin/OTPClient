#include <glib.h>
#include <jansson.h>
#include <cotp.h>
#include <glib/gi18n.h>
#include "../common/db-common.h"
#include "get-data.h"

static gint compare_strings (const gchar    *s1,
                             const gchar    *s2,
                             gboolean        match_exactly);

static void emit_token      (json_t         *obj,
                             DatabaseData   *db_data,
                             gboolean        show_next_token,
                             OutputFormat    format,
                             json_t         *json_out_array);

static void csv_append_field (GString *out, const gchar *value);


gboolean
show_token (DatabaseData *db_data,
            const gchar  *account,
            const gchar  *issuer,
            gboolean      match_exactly,
            gboolean      show_next_token,
            OutputFormat  format)
{
    gsize index;
    json_t *obj;
    gboolean found = FALSE;

    /* For machine-readable formats we collect rows first, then emit a single
     * well-formed document at the end. */
    json_t *json_rows = NULL;
    GString *csv = NULL;
    if (format == OUTPUT_FORMAT_JSON) {
        json_rows = json_array ();
    } else if (format == OUTPUT_FORMAT_CSV) {
        csv = g_string_new ("type,account,issuer,current,validity_seconds,next\n");
    }

    json_array_foreach (db_data->in_memory_json_data, index, obj) {
        const gchar *account_from_db = json_string_value (json_object_get (obj, "label"));
        const gchar *issuer_from_db = NULL;
        if (issuer != NULL) {
            issuer_from_db = json_string_value (json_object_get (obj, "issuer"));
        }
        gboolean match = FALSE;
        if (account_from_db != NULL && issuer_from_db != NULL && account != NULL) {
            match = (compare_strings (account_from_db, account, match_exactly) == 0 &&
                     compare_strings (issuer_from_db, issuer, match_exactly) == 0);
        } else if (account_from_db != NULL && account != NULL) {
            match = (compare_strings (account_from_db, account, match_exactly) == 0);
        } else if (issuer_from_db != NULL && issuer != NULL) {
            match = (compare_strings (issuer_from_db, issuer, match_exactly) == 0);
        }
        if (!match)
            continue;

        if (format == OUTPUT_FORMAT_CSV) {
            /* Build a transient json row, then serialize from it for consistency. */
            json_t *row = json_object ();
            emit_token (obj, db_data, show_next_token, OUTPUT_FORMAT_JSON, row);

            csv_append_field (csv, json_string_value (json_object_get (row, "type")));
            g_string_append_c (csv, ',');
            csv_append_field (csv, json_string_value (json_object_get (row, "account")));
            g_string_append_c (csv, ',');
            csv_append_field (csv, json_string_value (json_object_get (row, "issuer")));
            g_string_append_c (csv, ',');
            csv_append_field (csv, json_string_value (json_object_get (row, "current")));
            g_string_append_c (csv, ',');
            json_t *vs = json_object_get (row, "validity_seconds");
            if (json_is_integer (vs)) {
                g_string_append_printf (csv, "%lld", (long long) json_integer_value (vs));
            }
            g_string_append_c (csv, ',');
            csv_append_field (csv, json_string_value (json_object_get (row, "next")));
            g_string_append_c (csv, '\n');
            json_decref (row);
        } else {
            emit_token (obj, db_data, show_next_token, format, json_rows);
        }
        found = TRUE;
    }

    if (!found) {
        if (format == OUTPUT_FORMAT_JSON) {
            g_print ("[]\n");
            json_decref (json_rows);
            return FALSE;
        }
        if (format == OUTPUT_FORMAT_CSV) {
            g_print ("%s", csv->str);
            g_string_free (csv, TRUE);
            return FALSE;
        }
        g_printerr ("%s\n", _("Couldn't find the data. Either the given data is wrong or is not in the database."));

        // Translators: please do not translate 'account'
        GString *msg = g_string_new (_("Given account: %s"));
        g_string_replace (msg, "%s", account != NULL ? account : "<none>", 0);
        g_printerr ("%s\n", msg->str);
        g_string_free (msg, TRUE);

        // Translators: please do not translate 'issuer'
        msg = g_string_new (_("Given issuer: %s"));
        g_string_replace (msg, "%s", issuer != NULL ? issuer : "<none>", 0);
        g_printerr ("%s\n", msg->str);
        g_string_free (msg, TRUE);
        return FALSE;
    }

    if (format == OUTPUT_FORMAT_JSON) {
        char *dumped = json_dumps (json_rows, JSON_INDENT (2));
        g_print ("%s\n", dumped);
        gcry_free (dumped);
        json_decref (json_rows);
    } else if (format == OUTPUT_FORMAT_CSV) {
        g_print ("%s", csv->str);
        g_string_free (csv, TRUE);
    }
    return TRUE;
}


void
list_all_acc_iss (DatabaseData *db_data,
                  OutputFormat  format)
{
    gsize index;
    json_t *obj;

    if (format == OUTPUT_FORMAT_JSON) {
        json_t *arr = json_array ();
        json_array_foreach (db_data->in_memory_json_data, index, obj) {
            json_t *row = json_object ();
            const gchar *label = json_string_value (json_object_get (obj, "label"));
            const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
            const gchar *group = json_string_value (json_object_get (obj, "group"));
            const gchar *type = json_string_value (json_object_get (obj, "type"));
            json_object_set_new (row, "account", json_string (label ? label : ""));
            json_object_set_new (row, "issuer", json_string (issuer ? issuer : ""));
            json_object_set_new (row, "group", json_string (group ? group : ""));
            json_object_set_new (row, "type", json_string (type ? type : ""));
            json_array_append_new (arr, row);
        }
        char *dumped = json_dumps (arr, JSON_INDENT (2));
        g_print ("%s\n", dumped);
        gcry_free (dumped);
        json_decref (arr);
        return;
    }

    if (format == OUTPUT_FORMAT_CSV) {
        GString *csv = g_string_new ("account,issuer,group,type\n");
        json_array_foreach (db_data->in_memory_json_data, index, obj) {
            csv_append_field (csv, json_string_value (json_object_get (obj, "label")));
            g_string_append_c (csv, ',');
            csv_append_field (csv, json_string_value (json_object_get (obj, "issuer")));
            g_string_append_c (csv, ',');
            csv_append_field (csv, json_string_value (json_object_get (obj, "group")));
            g_string_append_c (csv, ',');
            csv_append_field (csv, json_string_value (json_object_get (obj, "type")));
            g_string_append_c (csv, '\n');
        }
        g_print ("%s", csv->str);
        g_string_free (csv, TRUE);
        return;
    }

    g_print ("=========================\n");
    g_print ("Account | Issuer | Group\n");
    g_print ("=========================\n");
    json_array_foreach (db_data->in_memory_json_data, index, obj) {
        const gchar *label = json_string_value (json_object_get (obj, "label"));
        const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
        const gchar *group = json_string_value (json_object_get (obj, "group"));
        g_print ("%s | %s | %s\n",
                 label ? label : "",
                 issuer ? issuer : "",
                 group ? group : "");
        g_print ("-------------------------\n");
    }
}


static gint
compare_strings (const gchar *s1,
                 const gchar *s2,
                 gboolean     match_exactly)
{
    return match_exactly ? g_strcmp0 (s1, s2) : g_ascii_strcasecmp (s1, s2);
}


/* Emit one --show row. For TABLE format, prints to stdout in the legacy layout.
 * For JSON format, populates `dest`: if `dest` is an array, appends a new object;
 * if it's an object, fills it in place. */
static void
emit_token (json_t       *obj,
            DatabaseData *db_data,
            gboolean      show_next_token,
            OutputFormat  format,
            json_t       *dest)
{
    cotp_error_t cotp_err;
    const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
    const gchar *label = json_string_value (json_object_get (obj, "label"));
    const gchar *secret = json_string_value (json_object_get (obj, "secret"));
    gint digits = (gint)json_integer_value (json_object_get (obj, "digits"));
    gint algo = get_algo_int_from_str (json_string_value (json_object_get (obj, "algo")));
    const gchar *type = json_string_value (json_object_get (obj, "type"));
    if (type == NULL) {
        if (format == OUTPUT_FORMAT_TABLE)
            g_printerr ("[ERROR] Token has no type field, skipping.\n");
        return;
    }
    if (secret == NULL) {
        if (format == OUTPUT_FORMAT_TABLE)
            g_printerr ("[ERROR] Token has no secret field, skipping.\n");
        return;
    }

    json_t *row = NULL;
    if (format == OUTPUT_FORMAT_JSON) {
        row = json_is_array (dest) ? json_object () : dest;
        json_object_set_new (row, "type", json_string (type));
        json_object_set_new (row, "account", json_string (label ? label : ""));
        json_object_set_new (row, "issuer", json_string (issuer ? issuer : ""));
    }

    if (g_ascii_strcasecmp (type, "TOTP") == 0) {
        gint period = (gint)json_integer_value (json_object_get (obj, "period"));
        glong current_ts = time (NULL);
        gint token_validity = period - (gint) (current_ts % period);
        gchar *current_totp = NULL;
        gchar *next_totp = NULL;
        if ((issuer != NULL && g_ascii_strcasecmp (issuer, "steam") == 0) ? TRUE : FALSE) {
            current_totp = get_steam_totp_at (secret, current_ts, period, &cotp_err);
            if (cotp_err != NO_ERROR) {
                if (format == OUTPUT_FORMAT_TABLE)
                    g_printerr ("[ERROR] Failed to generate Steam TOTP (error %d).\n", cotp_err);
                free (current_totp);
                if (row != NULL && json_is_array (dest)) json_decref (row);
                return;
            }
            if (show_next_token) {
                next_totp = get_steam_totp_at (secret, current_ts + period, period, &cotp_err);
                if (cotp_err != NO_ERROR) {
                    if (format == OUTPUT_FORMAT_TABLE)
                        g_printerr ("[ERROR] Failed to generate next Steam TOTP (error %d).\n", cotp_err);
                    free (current_totp);
                    free (next_totp);
                    if (row != NULL && json_is_array (dest)) json_decref (row);
                    return;
                }
            }
        } else {
            current_totp = get_totp_at (secret, current_ts, digits, period, algo, &cotp_err);
            if (cotp_err != NO_ERROR) {
                if (format == OUTPUT_FORMAT_TABLE)
                    g_printerr ("[ERROR] Failed to generate TOTP (error %d).\n", cotp_err);
                free (current_totp);
                if (row != NULL && json_is_array (dest)) json_decref (row);
                return;
            }
            if (show_next_token) {
                next_totp = get_totp_at (secret, current_ts + period, digits, period, algo, &cotp_err);
                if (cotp_err != NO_ERROR) {
                    if (format == OUTPUT_FORMAT_TABLE)
                        g_printerr ("[ERROR] Failed to generate next TOTP (error %d).\n", cotp_err);
                    free (current_totp);
                    free (next_totp);
                    if (row != NULL && json_is_array (dest)) json_decref (row);
                    return;
                }
            }
        }
        if (format == OUTPUT_FORMAT_TABLE) {
            g_print (_("Current TOTP (valid for %d more second(s)): %s\n"), token_validity, current_totp);
            if (show_next_token) g_print ("Next TOTP: %s\n", next_totp);
        } else if (row != NULL) {
            json_object_set_new (row, "current", json_string (current_totp));
            json_object_set_new (row, "validity_seconds", json_integer (token_validity));
            if (show_next_token && next_totp != NULL)
                json_object_set_new (row, "next", json_string (next_totp));
        }
        free (current_totp);
        free (next_totp);
    } else {
        gint64 counter = json_integer_value (json_object_get (obj, "counter"));
        gchar *hotp = get_hotp (secret, counter, digits, algo, &cotp_err);
        if (cotp_err != NO_ERROR) {
            if (format == OUTPUT_FORMAT_TABLE)
                g_printerr ("[ERROR] Failed to generate HOTP (error %d).\n", cotp_err);
            free (hotp);
            if (row != NULL && json_is_array (dest)) json_decref (row);
            return;
        }
        if (format == OUTPUT_FORMAT_TABLE) {
            g_print (_("Current HOTP: %s\n"), hotp);
        } else if (row != NULL) {
            json_object_set_new (row, "current", json_string (hotp));
            json_object_set_new (row, "counter", json_integer (counter + 1));
        }
        free (hotp);
        // counter must be updated every time it is accessed
        json_object_set (obj, "counter", json_integer (counter + 1));
        GError *err = NULL;
        update_db (db_data, &err);
        if (err != NULL) {
            g_printerr ("[ERROR] %s\n", err->message);
        } else {
            reload_db (db_data, &err);
            if (err != NULL) {
                g_printerr ("[ERROR] %s\n", err->message);
            }
        }
    }

    if (row != NULL && json_is_array (dest))
        json_array_append_new (dest, row);
}


/* RFC 4180-style CSV escaping: wrap in quotes when the field contains
 * a comma, quote, CR, or LF; double any embedded quote. */
static void
csv_append_field (GString *out, const gchar *value)
{
    if (value == NULL || value[0] == '\0')
        return;

    gboolean needs_quoting = (strpbrk (value, ",\"\r\n") != NULL);
    if (!needs_quoting) {
        g_string_append (out, value);
        return;
    }
    g_string_append_c (out, '"');
    for (const gchar *p = value; *p != '\0'; p++) {
        if (*p == '"')
            g_string_append (out, "\"\"");
        else
            g_string_append_c (out, *p);
    }
    g_string_append_c (out, '"');
}
