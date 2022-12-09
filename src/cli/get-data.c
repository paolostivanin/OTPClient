#include <glib.h>
#include <jansson.h>
#include <cotp.h>
#include <glib/gi18n.h>
#include "../db-misc.h"
#include "../common/common.h"

static gint compare_strings (const gchar    *s1,
                             const gchar    *s2,
                             gboolean        match_exactly);

static void get_token       (json_t         *obj,
                             DatabaseData   *db_data,
                             gboolean        show_next_token);


void
show_token (DatabaseData *db_data,
            const gchar  *account,
            const gchar  *issuer,
            gboolean      match_exactly,
            gboolean      show_next_token)
{
    gsize index;
    json_t *obj;
    gboolean found = FALSE;
    json_array_foreach (db_data->json_data, index, obj) {
        const gchar *account_from_db = json_string_value (json_object_get (obj, "label"));
        const gchar *issuer_from_db = NULL;
        if (issuer != NULL) {
            issuer_from_db = json_string_value (json_object_get (obj, "issuer"));
        }
        if (account_from_db != NULL && issuer_from_db != NULL && account != NULL) {
            // both account and issuer are present
            if (compare_strings (account_from_db, account, match_exactly) == 0 && compare_strings (issuer_from_db, issuer, match_exactly) == 0) {
                get_token (obj, db_data, show_next_token);
                found = TRUE;
            }
        } else {
            if (account_from_db != NULL && account != NULL) {
                // account is present, but issuer is not
                if (compare_strings (account_from_db, account, match_exactly) == 0) {
                    get_token (obj, db_data, show_next_token);
                    found = TRUE;
                }
            } else {
                // account was null, but issue may be present
                if (issuer_from_db != NULL) {
                    if (compare_strings (issuer_from_db, issuer, match_exactly) == 0) {
                        get_token (obj, db_data, show_next_token);
                        found = TRUE;
                    }
                }
            }
        }
    }
    if (!found) {
        g_printerr ("%s\n", _("Couldn't find the data. Either the given data is wrong or is not in the database."));

        // Translators: please do not translate 'account'
        GString *msg = g_string_new (_("Given account: %s"));
#if GLIB_CHECK_VERSION(2, 68, 0)
        g_string_replace (msg, "%s", account != NULL ? account : "<none>", 0);
#else
        g_string_replace_backported (msg, "%s", account != NULL ? account : "<none>", 0);
#endif
        g_printerr ("%s\n", msg->str);
        g_string_free (msg, TRUE);

        // Translators: please do not translate 'issuer'
        msg = g_string_new (_("Given issuer: %s"));
#if GLIB_CHECK_VERSION(2, 68, 0)
        g_string_replace (msg, "%s", issuer != NULL ? issuer : "<none>", 0);
#else
        g_string_replace_backported (msg, "%s", issuer != NULL ? issuer : "<none>", 0);
#endif
        g_printerr ("%s\n", msg->str);
        g_string_free (msg, TRUE);

        return;
    }
}


void
list_all_acc_iss (DatabaseData *db_data)
{
    gsize index;
    json_t *obj;
    g_print ("================\n");
    g_print ("Account | Issuer\n");
    g_print ("================\n");
    json_array_foreach (db_data->json_data, index, obj) {
        g_print ("%s | %s\n", json_string_value (json_object_get (obj, "label")), json_string_value (json_object_get (obj, "issuer")));
        g_print ("----------------\n");
    }
}


static gint
compare_strings (const gchar *s1,
                 const gchar *s2,
                 gboolean     match_exactly)
{
    return match_exactly ? g_strcmp0 (s1, s2) : g_ascii_strcasecmp (s1, s2);
}


static void
get_token (json_t       *obj,
           DatabaseData *db_data,
           gboolean      show_next_token)
{
    cotp_error_t cotp_err;
    const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
    const gchar *secret = json_string_value (json_object_get (obj, "secret"));
    gint digits = (gint)json_integer_value (json_object_get (obj, "digits"));
    gint algo = get_algo_int_from_str (json_string_value (json_object_get (obj, "algo")));
    gint period;
    gint64 counter;
    if (g_ascii_strcasecmp (json_string_value (json_object_get (obj, "type")), "TOTP") == 0) {
        period = (gint)json_integer_value (json_object_get (obj, "period"));
        gint remaining_seconds = (period > 59 ? 119 : 59) - g_date_time_get_second (g_date_time_new_now_local());
        gint token_validity = remaining_seconds % period;
        glong current_ts = time(NULL);
        gchar *current_totp = NULL;
        gchar *next_totp = NULL;
        if ((issuer != NULL && g_ascii_strcasecmp (issuer, "steam") == 0) ? TRUE : FALSE) {
            current_totp = get_steam_totp_at (secret, current_ts, period, &cotp_err);
            if (show_next_token) next_totp = get_steam_totp_at (secret, current_ts + period, period, &cotp_err);
        } else {
            current_totp = get_totp_at (secret, current_ts, digits, period, algo, &cotp_err);
            if (show_next_token) next_totp = get_totp_at (secret, current_ts + period, digits, period, algo, &cotp_err);
        }
        g_print (_("Current TOTP (valid for %d more second(s)): %s\n"), token_validity, current_totp);
        if (show_next_token) g_print ("Next TOTP: %s\n", next_totp);
    } else {
        counter = json_integer_value (json_object_get (obj, "counter"));
        g_print (_("Current HOTP: %s\n"), get_hotp (secret, counter, digits, algo, &cotp_err));
        // counter must be updated every time it is accessed
        json_object_set (obj, "counter", json_integer (counter + 1));
        GError *err = NULL;
        update_and_reload_db (NULL, db_data, FALSE, &err);
        if (err != NULL) {
            g_printerr ("[ERROR] %s\n", err->message);
        }
    }
}
