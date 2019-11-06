#include <glib.h>
#include <jansson.h>
#include <cotp.h>
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
        if (compare_strings (json_string_value (json_object_get (obj, "label")), account, match_exactly) == 0){
            if (issuer != NULL) {
                if (compare_strings (json_string_value (json_object_get (obj, "issuer")), issuer, match_exactly) == 0) {
                    get_token (obj, db_data, show_next_token);
                    found = TRUE;
                }
            } else {
                get_token (obj, db_data, show_next_token);
                found = TRUE;
            }
        }
    }
    if (!found) {
        g_printerr ("Couldn't find the data. Either the given data is wrong or is not in the database.\n");
        g_printerr ("Given account: %s\n", account);
        if (issuer != NULL) g_printerr ("Given issuer: %s\n", issuer);
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
    gint digits = json_integer_value (json_object_get (obj, "digits"));
    gint algo = get_algo_int_from_str (json_string_value (json_object_get (obj, "algo")));
    gint period;
    gint64 counter;
    if (g_strcmp0 (json_string_value (json_object_get (obj, "type")), "TOTP") == 0) {
        period = json_integer_value (json_object_get (obj, "period"));
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
        g_print ("Current TOTP (valid for %d more second(s)): %s\n", token_validity, current_totp);
        if (show_next_token) g_print ("Next TOTP: %s\n", next_totp);
    } else {
        counter = json_integer_value (json_object_get (obj, "counter"));
        g_print ("Current HOTP: %s\n", get_hotp (secret, counter, digits, algo, &cotp_err));
        // counter must be updated every time it is accessed
        json_object_set (obj, "counter", json_integer (counter + 1));
        GError *err = NULL;
        update_and_reload_db (NULL, db_data, FALSE, &err);
        if (err != NULL) {
            g_printerr ("[ERROR] %s\n", err->message);
        }
    }
}
