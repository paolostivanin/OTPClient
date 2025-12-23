#include "gtk-compat.h"
#include <jansson.h>
#include <cotp.h>
#include "message-dialogs.h"
#include "add-common.h"
#include "gui-misc.h"
#include "../common/common.h"
#include "google-migration.pb-c.h"
#include "../common/gquarks.h"
#include "../common/macros.h"
#include "treeview.h"


void
icon_press_cb (GtkEntry         *entry,
               gint              position UNUSED,
               GdkEvent         *event UNUSED,
               gpointer          data UNUSED)
{
    gtk_entry_set_visibility (GTK_ENTRY (entry), !gtk_entry_get_visibility (entry));
}


guint
get_row_number_from_iter (GtkListStore *list_store,
                          GtkTreeIter iter)
{
    GtkTreePath *path = gtk_tree_model_get_path (GTK_TREE_MODEL(list_store), &iter);
    gint *row_number = gtk_tree_path_get_indices (path); // starts from 0
    guint row = (guint)row_number[0];
    gtk_tree_path_free (path);

    return row;
}


void
send_ok_cb (GtkWidget *entry,
            gpointer   user_data UNUSED)
{
    GtkWidget *toplevel = gtk_widget_get_toplevel (entry);
    if (toplevel != NULL) {
        gtk_dialog_response (GTK_DIALOG(toplevel), GTK_RESPONSE_OK);
    }
}


gchar *
parse_uris_migration (AppData  *app_data,
                      const     gchar *user_uri,
                      gboolean  google_migration)
{
    gchar *return_err_msg = NULL;
    GSList *otpauth_decoded_uris = NULL;
    if (google_migration == TRUE) {
        gint failed = 0;
        otpauth_decoded_uris = decode_migration_data (user_uri);
        for (gint i = 0; i < g_slist_length (otpauth_decoded_uris); i++) {
            gchar *uri = g_slist_nth_data (otpauth_decoded_uris, i);
            gchar *err_msg = add_data_to_db (uri, app_data);
            if (err_msg != NULL) {
                failed++;
                g_free (err_msg);
            }
        }
        if (failed > 0) {
            GString *e_msg = g_string_new (NULL);
            g_string_printf (e_msg, "Failed to add all OTPs. Only %u out of %u were successfully added.", g_slist_length (otpauth_decoded_uris) - failed,
                             g_slist_length (otpauth_decoded_uris));
            return_err_msg = g_strdup (e_msg->str);
            g_string_free (e_msg, TRUE);
        }
        g_slist_free_full (otpauth_decoded_uris, g_free);
    } else {
        return_err_msg = add_data_to_db (user_uri, app_data);
    }

    return return_err_msg;
}


gchar *
g_trim_whitespace (const gchar *str)
{
    if (g_utf8_strlen (str, -1) == 0) {
        return NULL;
    }
    gchar *sec_buf = gcry_calloc_secure (strlen (str) + 1, 1);
    int pos = 0;
    for (int i = 0; str[i]; i++) {
        if (str[i] != ' ') {
            sec_buf[pos++] = str[i];
        }
    }
    sec_buf[pos] = '\0';
    gchar *secubf_newpos = (gchar *)gcry_realloc (sec_buf, strlen (sec_buf) + 1);

    return secubf_newpos;
}


// Backported from Glib (needed by below function)
static int
unescape_character (const char *scanner)
{
    int first_digit;
    int second_digit;

    first_digit = g_ascii_xdigit_value (*scanner++);
    if (first_digit < 0)
        return -1;

    second_digit = g_ascii_xdigit_value (*scanner++);
    if (second_digit < 0)
        return -1;

    return (first_digit << 4) | second_digit;
}


// Backported from Glib. The only difference is that it's using gcrypt to allocate a secure buffer.
gchar *
g_uri_unescape_string_secure (const gchar *escaped_string,
                              const gchar *illegal_characters)
{
    if (escaped_string == NULL)
        return NULL;

    const gchar *escaped_string_end = escaped_string + g_utf8_strlen (escaped_string, -1);

    gchar *result = gcry_calloc_secure (escaped_string_end - escaped_string + 1, 1);
    gchar *out = result;

    const gchar *in;
    gint character;
    for (in = escaped_string; in < escaped_string_end; in++) {
        character = *in;

        if (*in == '%') {
            in++;
            if (escaped_string_end - in < 2) {
                // Invalid escaped char (to short)
                gcry_free (result);
                return NULL;
            }

            character = unescape_character (in);

            // Check for an illegal character. We consider '\0' illegal here.
            if (character <= 0 ||
                (illegal_characters != NULL &&
                 strchr (illegal_characters, (char)character) != NULL)) {
                gcry_free (result);
                return NULL;
            }

            in++; // The other char will be eaten in the loop header
        }
        *out++ = (char)character;
    }

    *out = '\0';

    return result;
}


guchar *
g_base64_decode_secure (const gchar *text,
                        gsize       *out_len)
{
    guchar *ret;
    gsize input_length;
    gint state = 0;
    guint save = 0;

    g_return_val_if_fail (text != NULL, NULL);
    g_return_val_if_fail (out_len != NULL, NULL);

    input_length = g_utf8_strlen (text, -1);

    /* We can use a smaller limit here, since we know the saved state is 0,
       +1 used to avoid calling g_malloc0(0), and hence returning NULL */
    ret = gcry_calloc_secure ((input_length / 4) * 3 + 1, 1);

    *out_len = g_base64_decode_step (text, input_length, ret, &state, &save);

    return ret;
}


GSList *
decode_migration_data (const gchar *encoded_uri)
{
    const gchar *encoded_uri_copy = encoded_uri;
    if (g_ascii_strncasecmp (encoded_uri_copy, "otpauth-migration://offline?data=", 33) != 0) {
        return NULL;
    }
    encoded_uri_copy += 33;
    gsize out_len = 0;
    gchar *unesc_str = g_uri_unescape_string_secure (encoded_uri_copy, NULL);
    guchar *data = g_base64_decode_secure (unesc_str, &out_len);
    gcry_free (unesc_str);

    GSList *uris = NULL;
    GString *uri = NULL;
    MigrationPayload *msg = migration_payload__unpack (NULL, out_len, data);
    gcry_free (data);
    for (gint i = 0; i < msg->n_otp_parameters; i++) {
        uri = g_string_new ("otpauth://");
        if (msg->otp_parameters[i]->type == 1) {
            g_string_append (uri, "hotp/");
        } else if (msg->otp_parameters[i]->type == 2) {
            g_string_append (uri, "totp/");
        } else {
            g_printerr ("OTP type not recognized, skipping %s\n", msg->otp_parameters[i]->name);
            goto end;
        }

        g_string_append (uri, msg->otp_parameters[i]->name);
        g_string_append (uri, "?");

        if (msg->otp_parameters[i]->algorithm == 1) {
            g_string_append (uri, "algorithm=SHA1&");
        } else if (msg->otp_parameters[i]->algorithm == 2) {
            g_string_append (uri, "algorithm=SHA256&");
        } else if (msg->otp_parameters[i]->algorithm == 3) {
            g_string_append (uri, "algorithm=SHA512&");
        } else {
            g_printerr ("Algorithm type not supported, skipping %s\n", msg->otp_parameters[i]->name);
            goto end;
        }

        if (msg->otp_parameters[i]->digits == 1) {
            g_string_append (uri, "digits=6&");
        } else if (msg->otp_parameters[i]->digits == 2) {
            g_string_append (uri, "digits=8&");
        } else {
            g_printerr ("Algorithm type not supported, skipping %s\n", msg->otp_parameters[i]->name);
            goto end;
        }

        if (msg->otp_parameters[i]->issuer != NULL) {
            g_string_append (uri, "issuer=");
            g_string_append (uri, msg->otp_parameters[i]->issuer);
            g_string_append (uri, "&");
        }

        if (msg->otp_parameters[i]->type == 1) {
            g_string_append (uri, "counter=");
            g_string_append_printf(uri, "%ld", msg->otp_parameters[i]->counter);
            g_string_append (uri, "&");
        }

        cotp_error_t b_err;
        gchar *b32_encoded_secret = base32_encode (msg->otp_parameters[i]->secret.data, msg->otp_parameters[i]->secret.len, &b_err);
        if (b32_encoded_secret == NULL) {
            g_printerr ("Error while encoding the secret (error code %d)\n", b_err);
            goto end;
        }

        g_string_append (uri, "secret=");
        g_string_append (uri, b32_encoded_secret);

        uris = g_slist_append (uris, g_strdup (uri->str));

        end:
        g_string_free (uri, TRUE);
    }

    migration_payload__free_unpacked (msg, NULL);

    return uris;
}


gchar *
update_db_from_otps (GSList *otps, AppData *app_data)
{
    add_otps_to_db (otps, app_data->db_data);

    GError *err = NULL;
    update_db (app_data->db_data, &err);
    if (err != NULL && !g_error_matches (err, missing_file_gquark (), MISSING_FILE_ERRCODE)) {
        return g_strdup (err->message);
    }
    reload_db (app_data->db_data, &err);
    if (err != NULL && !g_error_matches (err, missing_file_gquark (), MISSING_FILE_ERRCODE)) {
        return g_strdup (err->message);
    }
    regenerate_model (app_data);

    return NULL;
}


void
load_new_db (AppData  *app_data,
             GError  **err)
{
    reload_db (app_data->db_data, err);
    if (*err != NULL) {
        return;
    }

    update_model (app_data);
    g_slist_free_full (app_data->db_data->data_to_add, json_free);
    app_data->db_data->data_to_add = NULL;
}


gboolean
get_selected_liststore_iter (AppData       *app_data,
                             GtkListStore **list_store,
                             GtkTreeIter   *iter)
{
    GtkTreeModel *model = gtk_tree_view_get_model (app_data->tree_view);
    GtkTreeIter view_iter;
    if (!gtk_tree_selection_get_selected (gtk_tree_view_get_selection (app_data->tree_view), &model, &view_iter)) {
        return FALSE;
    }

    if (GTK_IS_TREE_MODEL_FILTER (model)) {
        GtkTreeIter child_iter;
        GtkTreeModel *child_model = gtk_tree_model_filter_get_model (GTK_TREE_MODEL_FILTER(model));
        gtk_tree_model_filter_convert_iter_to_child_iter (GTK_TREE_MODEL_FILTER(model), &child_iter, &view_iter);
        *list_store = GTK_LIST_STORE(child_model);
        *iter = child_iter;
        return TRUE;
    }

    *list_store = GTK_LIST_STORE(model);
    *iter = view_iter;
    return TRUE;
}
