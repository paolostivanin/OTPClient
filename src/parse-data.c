#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include "otpclient.h"
#include "add-data-dialog.h"
#include "gquarks.h"


static gboolean is_input_valid (GtkWidget *dialog,
                                const gchar *acc_label, const gchar *acc_iss, const gchar *secret,
                                gint len, GError **err);

static gboolean str_is_only_num_or_alpha (const gchar *secret);

static JsonNode *get_json_node (Widgets *widgets, const gchar *acc_label, const gchar *acc_iss, const gchar *acc_key, gint i);

static JsonNode *build_json_node (const gchar *type, const gchar *acc_label, const gchar *acc_iss,
                                  const gchar *acc_key, const gchar *digits_str, const gchar *algo, gint64 ctr);


gboolean
parse_user_data (Widgets        *widgets,
                 DatabaseData   *db_data)
{
    GError *err = NULL;
    JsonNode *jn;
    gint i = 0;
    while (i < widgets->acc_entry->len) {
        const gchar *acc_label = gtk_entry_get_text (GTK_ENTRY (g_array_index (widgets->acc_entry, GtkWidget * , i)));
        const gchar *acc_iss = gtk_entry_get_text (GTK_ENTRY (g_array_index (widgets->iss_entry, GtkWidget * , i)));
        const gchar *acc_key = gtk_entry_get_text (GTK_ENTRY (g_array_index (widgets->key_entry, GtkWidget * , i)));
        if (is_input_valid (widgets->dialog, acc_label, acc_iss, acc_key, widgets->acc_entry->len, &err)) {
            jn = get_json_node (widgets, acc_label, acc_iss, acc_key, i);
            guint hash = json_object_hash (json_node_get_object (jn));
            if (g_slist_find_custom (db_data->objects_hash, GUINT_TO_POINTER (hash), check_duplicate) == NULL) {
                db_data->objects_hash = g_slist_append (db_data->objects_hash, g_memdup (&hash, sizeof (guint)));
                db_data->data_to_add = g_slist_append (db_data->data_to_add, jn);
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


static JsonNode *
get_json_node (Widgets *widgets,
               const gchar *acc_label,
               const gchar *acc_iss,
               const gchar *acc_key,
               gint i)
{
    gchar *type = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (g_array_index (widgets->type_cb_box, GtkWidget * , i)));
    gchar *digits = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (g_array_index (widgets->dig_cb_box, GtkWidget * , i)));
    gchar *algo = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (g_array_index (widgets->alg_cb_box, GtkWidget * , i)));
    gdouble ctr = gtk_spin_button_get_value (GTK_SPIN_BUTTON (g_array_index (widgets->spin_btn, GtkWidget * , i)));
    JsonNode *jn = build_json_node (type, acc_label, acc_iss, acc_key, digits, algo, (gint64) ctr);
    g_free (type);
    g_free (digits);
    g_free (algo);

    return jn;
}


static JsonNode *
build_json_node (const gchar *type,
                 const gchar *acc_label,
                 const gchar *acc_iss,
                 const gchar *acc_key,
                 const gchar *digits_str,
                 const gchar *algo,
                 gint64       ctr)
{
    JsonBuilder *jb = json_builder_new ();
    gint64 digits = g_ascii_strtoll (digits_str, NULL, 10);

    jb = json_builder_begin_object (jb);
    jb = json_builder_set_member_name (jb, "otp");
    jb = json_builder_add_string_value (jb, type);
    jb = json_builder_set_member_name (jb, "label");
    jb = json_builder_add_string_value (jb, acc_label);
    jb = json_builder_set_member_name (jb, "issuer");
    jb = json_builder_add_string_value (jb, acc_iss);
    jb = json_builder_set_member_name (jb, "secret");
    jb = json_builder_add_string_value (jb, acc_key);
    jb = json_builder_set_member_name (jb, "digits");
    jb = json_builder_add_int_value (jb, digits);
    jb = json_builder_set_member_name (jb, "algo");
    jb = json_builder_add_string_value (jb, algo);
    if (g_strcmp0 (type, "TOTP") == 0) {
        jb = json_builder_set_member_name (jb, "period");
        json_builder_add_int_value (jb, 30);
    } else {
        jb = json_builder_set_member_name (jb, "counter");
        json_builder_add_int_value (jb, ctr);
    }
    jb = json_builder_end_object (jb);

    JsonNode *jnode = json_builder_get_root (jb);
    g_object_unref (jb);

    return jnode;
}