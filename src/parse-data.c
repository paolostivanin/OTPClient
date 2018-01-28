#include <gtk/gtk.h>
#include <string.h>
#include <jansson.h>
#include "otpclient.h"
#include "add-data-dialog.h"
#include "gquarks.h"
#include "message-dialogs.h"
#include "common.h"


static gboolean  is_input_valid           (GtkWidget *dialog,
                                           const gchar *acc_label, const gchar *acc_iss, const gchar *secret,
                                           gint len, GError **err);

static gboolean  str_is_only_num_or_alpha (const gchar *secret);

static json_t   *get_json_obj             (Widgets *widgets,
                                           const gchar *acc_label, const gchar *acc_iss, const gchar *acc_key,
                                           gint i);

static json_t   *build_json_obj           (const gchar *type, const gchar *acc_label, const gchar *acc_iss,
                                           const gchar *acc_key, const gchar *digits_str, const gchar *algo, gint64 ctr);


gboolean
parse_user_data (Widgets        *widgets,
                 DatabaseData   *db_data)
{
    GError *err = NULL;
    json_t *obj;
    gint i = 0;
    while (i < widgets->acc_entry->len) {
        const gchar *acc_label = gtk_entry_get_text (GTK_ENTRY (g_array_index (widgets->acc_entry, GtkWidget * , i)));
        const gchar *acc_iss = gtk_entry_get_text (GTK_ENTRY (g_array_index (widgets->iss_entry, GtkWidget * , i)));
        const gchar *acc_key = gtk_entry_get_text (GTK_ENTRY (g_array_index (widgets->key_entry, GtkWidget * , i)));
        if (is_input_valid (widgets->dialog, acc_label, acc_iss, acc_key, widgets->acc_entry->len, &err)) {
            obj = get_json_obj (widgets, acc_label, acc_iss, acc_key, i);
            guint32 hash = json_object_get_hash (obj);
            if (g_slist_find_custom (db_data->objects_hash, GUINT_TO_POINTER (hash), check_duplicate) == NULL) {
                db_data->objects_hash = g_slist_append (db_data->objects_hash, g_memdup (&hash, sizeof (guint)));
                db_data->data_to_add = g_slist_append (db_data->data_to_add, obj);
            } else {
                g_print ("[INFO] Duplicate element not added\n");
            }
        } else if (err != NULL) {
            return FALSE;
        }
        i++;
    }
    g_clear_error (&err);
    return TRUE;
}


static gboolean
is_input_valid (GtkWidget    *dialog,
                const gchar  *acc_label,
                const gchar  *acc_iss,
                const gchar  *secret,
                gint          len,
                GError      **err)
{
    if (g_utf8_strlen (acc_label, -1) == 0 || g_utf8_strlen (secret, -1) == 0) {
        show_message_dialog (dialog, "Label and/or secret can't be empty", GTK_MESSAGE_ERROR);
        if (len == 1) {
            g_set_error (err, invalid_input_gquark (), -1, "No more entries to process");
        }
        return FALSE;
    }
    if (!g_str_is_ascii (acc_label) || !g_str_is_ascii (acc_iss)) {
        gchar *msg = g_strconcat ("Only ASCII characters are supported. Entry with label '",
                                  acc_label, "' will not be added.", NULL);
        show_message_dialog (dialog, msg, GTK_MESSAGE_ERROR);
        if (len == 1) {
            g_set_error (err, invalid_input_gquark (), -1, "No more entries to process");
        }
        g_free (msg);
        return FALSE;
    }
    if (!str_is_only_num_or_alpha (secret)) {
        gchar *msg = g_strconcat ("Secret can contain only characters from the english alphabet and numbers. Entry with label '",
                                  acc_label, "' will not be added.", NULL);
        show_message_dialog (dialog, msg, GTK_MESSAGE_ERROR);
        if (len == 1) {
            g_set_error (err, invalid_input_gquark (), -1, "No more entries to process");
        }
        g_free (msg);
        return FALSE;
    }
    return TRUE;
}


static gboolean
str_is_only_num_or_alpha (const gchar *secret)
{
    for (gint i = 0; i < strlen (secret); i++) {
        if (!g_ascii_isalnum (secret[i]) && !g_ascii_isalpha (secret[i])) {
            return FALSE;
        }
    }
    return TRUE;
}


static json_t *
get_json_obj (Widgets *widgets,
              const gchar *acc_label,
              const gchar *acc_iss,
              const gchar *acc_key,
              gint i)
{
    gchar *type = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (g_array_index (widgets->type_cb_box, GtkWidget * , i)));
    gchar *digits = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (g_array_index (widgets->dig_cb_box, GtkWidget * , i)));
    gchar *algo = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (g_array_index (widgets->alg_cb_box, GtkWidget * , i)));
    gdouble ctr = gtk_spin_button_get_value (GTK_SPIN_BUTTON (g_array_index (widgets->spin_btn, GtkWidget * , i)));
    json_t *jn = build_json_obj (type, acc_label, acc_iss, acc_key, digits, algo, (gint64) ctr);
    g_free (type);
    g_free (digits);
    g_free (algo);

    return jn;
}


static json_t *
build_json_obj (const gchar *type,
                const gchar *acc_label,
                const gchar *acc_iss,
                const gchar *acc_key,
                const gchar *digits_str,
                const gchar *algo,
                gint64       ctr)
{
    json_t *obj = json_object ();
    gint64 digits = g_ascii_strtoll (digits_str, NULL, 10);
    json_object_set (obj, "type", json_string (type));
    json_object_set (obj, "label", json_string (acc_label));
    json_object_set (obj, "issuer", json_string (acc_iss));
    json_object_set (obj, "secret", json_string (acc_key));
    json_object_set (obj, "digits", json_integer (digits));
    json_object_set (obj, "algo", json_string (algo));

    json_object_set (obj, "secret", json_string (acc_key));

    if (g_strcmp0 (type, "TOTP") == 0) {
        json_object_set (obj, "period", json_integer (30));
    } else {
        json_object_set (obj, "counter", json_integer (ctr));
    }

    return obj;
}