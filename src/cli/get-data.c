#include <glib.h>
#include <jansson.h>
#include <cotp.h>
#include <glib/gi18n.h>
#include <stdlib.h>
#include <limits.h>
#include "../common/db-common.h"
#include "../common/otp-validation.h"
#include "get-data.h"

static gint compare_strings (const gchar    *s1,
                             const gchar    *s2,
                             gboolean        match_exactly);

static gboolean fill_totp_row (json_t *obj, gboolean show_next, json_t *row);

gboolean
show_token (DatabaseData *db_data, const gchar *account, const gchar *issuer,
            gboolean match_exactly, gboolean show_next_token, OutputFormat format)
{
    json_t *rows = json_array ();
    g_autoptr (GArray) indices = g_array_new (FALSE, FALSE, sizeof (gsize));
    g_autoptr (GArray) positions = g_array_new (FALSE, FALSE, sizeof (gsize));
    gboolean ok = TRUE;
    gsize index;
    json_t *obj;
    json_array_foreach (db_data->in_memory_json_data, index, obj) {
        const gchar *label = json_string_value (json_object_get (obj, "label"));
        const gchar *iss = json_string_value (json_object_get (obj, "issuer"));
        if ((account == NULL && issuer == NULL) ||
            (account != NULL && (label == NULL || compare_strings (label, account, match_exactly) != 0)) ||
            (issuer != NULL && (iss == NULL || compare_strings (iss, issuer, match_exactly) != 0)))
            continue;
        GError *err = NULL;
        if (!otp_validate_token_object (obj, index, &err)) {
            g_printerr ("%s\n", err != NULL ? err->message : _("Invalid token"));
            g_clear_error (&err);
            ok = FALSE;
            break;
        }
        const gchar *type = json_string_value (json_object_get (obj, "type"));
        json_t *row = json_object ();
        json_object_set_new (row, "type", json_string (type));
        json_object_set_new (row, "account", json_string (label ? label : ""));
        json_object_set_new (row, "issuer", json_string (iss ? iss : ""));
        if (g_ascii_strcasecmp (type, "HOTP") == 0) {
            gsize pos = json_array_size (rows);
            g_array_append_val (indices, index);
            g_array_append_val (positions, pos);
        } else if (!fill_totp_row (obj, show_next_token, row)) {
            ok = FALSE;
        }
        json_array_append_new (rows, row);
        if (!ok)
            break;
    }
    if (ok && indices->len > 0) {
        GError *err = NULL;
        g_autoptr (GPtrArray) results = db_generate_hotp (db_data,
            (const gsize *) indices->data, indices->len, &err);
        if (results == NULL) {
            g_printerr ("%s\n", err != NULL ? err->message : _("Failed to save HOTP counters"));
            g_clear_error (&err);
            ok = FALSE;
        } else {
            for (guint i = 0; i < results->len; i++) {
                DbHotpResult *result = g_ptr_array_index (results, i);
                json_t *row = json_array_get (rows, g_array_index (positions, gsize, i));
                json_object_set_new (row, "current", json_string (result->code));
                json_object_set_new (row, "counter", json_integer ((json_int_t) result->next_counter));
            }
        }
    }
    if (!ok) {
        json_decref (rows);
        return FALSE;
    }
    gboolean found = json_array_size (rows) > 0;
    if (format == OUTPUT_FORMAT_JSON) {
        gchar *output = json_dumps (rows, JSON_INDENT (2));
        g_print ("%s\n", output);
        sensitive_secure_free (output);
    } else {
        GString *output = g_string_new (format == OUTPUT_FORMAT_CSV
            ? "type,account,issuer,current,validity_seconds,counter,next\n" : "");
        json_t *row;
        json_array_foreach (rows, index, row) {
            if (format == OUTPUT_FORMAT_CSV) {
                const gchar *fields[] = {"type", "account", "issuer", "current", "validity_seconds", "counter", "next"};
                for (guint i = 0; i < G_N_ELEMENTS (fields); i++) {
                    if (i > 0) g_string_append_c (output, ',');
                    json_t *value = json_object_get (row, fields[i]);
                    if (json_is_integer (value))
                        g_string_append_printf (output, "%lld", (long long) json_integer_value (value));
                    else
                        csv_append_field (output, json_string_value (value));
                }
                g_string_append_c (output, '\n');
            } else if (json_object_get (row, "counter") != NULL) {
                g_string_append_printf (output, _("Current HOTP: %s\n"), json_string_value (json_object_get (row, "current")));
            } else {
                gint remaining = (gint) json_integer_value (json_object_get (row, "validity_seconds"));
                g_string_append_printf (output,
                    ngettext ("Current TOTP (valid for %d more second): %s\n", "Current TOTP (valid for %d more seconds): %s\n", remaining),
                    remaining, json_string_value (json_object_get (row, "current")));
                const gchar *next = json_string_value (json_object_get (row, "next"));
                if (next != NULL) g_string_append_printf (output, _("Next TOTP: %s\n"), next);
            }
        }
        g_print ("%s", output->str);
        sensitive_g_free (g_string_free (output, FALSE));
    }
    if (!found)
        g_printerr ("%s\n", _("No matching token found."));
    json_decref (rows);
    return found;
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
    g_print ("%s", _("Account | Issuer | Group\n"));
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


static gboolean
fill_totp_row (json_t *obj, gboolean show_next, json_t *row)
{
    const gchar *secret = json_string_value (json_object_get (obj, "secret"));
    const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
    gint period = (gint) json_integer_value (json_object_get (obj, "period"));
    gint digits = (gint) json_integer_value (json_object_get (obj, "digits"));
    gint algo = get_algo_int_from_str (json_string_value (json_object_get (obj, "algo")));
    time_t now = time (NULL);
    if (now < 0 || period <= 0 || (guint64) now + period > (guint64) LONG_MAX) {
        g_printerr ("%s\n", _("Invalid TOTP time or period."));
        return FALSE;
    }
    gboolean steam = issuer != NULL && g_ascii_strcasecmp (issuer, "steam") == 0;
    for (guint i = 0; i < (show_next ? 2u : 1u); i++) {
        cotp_error_t err;
        long timestamp = (long) now + i * period;
        gchar *code = steam ? get_steam_totp_at (secret, timestamp, period, &err)
                            : get_totp_at (secret, timestamp, digits, period, algo, &err);
        if (code == NULL || err != NO_ERROR) {
            sensitive_free (code);
            g_printerr ("%s\n", _("Failed to generate TOTP."));
            return FALSE;
        }
        json_object_set_new (row, i == 0 ? "current" : "next", json_string (code));
        sensitive_free (code);
    }
    json_object_set_new (row, "validity_seconds", json_integer (period - now % period));
    return TRUE;
}


/* RFC 4180-style CSV escaping: wrap in quotes when the field contains
 * a comma, quote, CR, or LF; double any embedded quote. */
void
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
