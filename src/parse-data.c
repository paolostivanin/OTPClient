#include <gtk/gtk.h>
#include <string.h>
#include <jansson.h>
#include "otpclient.h"
#include "db-misc.h"
#include "manual-add-cb.h"
#include "gquarks.h"
#include "message-dialogs.h"
#include "gui-common.h"
#include "common/common.h"


static gboolean  is_input_valid            (GtkWidget   *dialog,
                                            const gchar *acc_label,
                                            const gchar *acc_iss,
                                            const gchar *secret,
                                            const gchar *digits,
                                            const gchar *period,
                                            gboolean     period_active,
                                            const gchar *counter,
                                            gboolean     counter_active);

static gboolean  str_is_only_num_or_alpha  (const gchar *string);

static gboolean  str_is_only_num           (const gchar *string);

static json_t   *get_json_obj              (Widgets     *widgets,
                                            const gchar *acc_label,
                                            const gchar *acc_iss,
                                            const gchar *acc_key,
                                            const gchar *digits,
                                            const gchar *period,
                                            const gchar *counter);


gboolean
parse_user_data (Widgets        *widgets,
                 DatabaseData   *db_data)
{
    json_t *obj;

    const gchar *acc_label = gtk_entry_get_text (GTK_ENTRY (widgets->label_entry));
    const gchar *acc_iss = gtk_entry_get_text (GTK_ENTRY (widgets->iss_entry));
    const gchar *acc_key = gtk_entry_get_text (GTK_ENTRY (widgets->sec_entry));
    const gchar *digits = gtk_entry_get_text (GTK_ENTRY (widgets->digits_entry));
    const gchar *period = gtk_entry_get_text (GTK_ENTRY (widgets->period_entry));
    const gchar *counter = gtk_entry_get_text (GTK_ENTRY (widgets->counter_entry));
    gboolean period_active = gtk_widget_get_sensitive (widgets->period_entry);
    gboolean counter_active = gtk_widget_get_sensitive (widgets->counter_entry);
    if (is_input_valid (widgets->dialog, acc_label, acc_iss, acc_key, digits, period, period_active, counter, counter_active)) {
        obj = get_json_obj (widgets, acc_label, acc_iss, acc_key, digits, period, counter);
        guint32 hash = json_object_get_hash (obj);
        if (g_slist_find_custom (db_data->objects_hash, GUINT_TO_POINTER (hash), check_duplicate) == NULL) {
            db_data->objects_hash = g_slist_append (db_data->objects_hash, g_memdup (&hash, sizeof (guint)));
            db_data->data_to_add = g_slist_append (db_data->data_to_add, obj);
        } else {
            g_print ("[INFO] Duplicate element not added\n");
        }
    } else {
        return FALSE;
    }
    return TRUE;
}


static gboolean
is_input_valid (GtkWidget   *dialog,
                const gchar *acc_label,
                const gchar *acc_iss,
                const gchar *secret,
                const gchar *digits,
                const gchar *period,
                gboolean     period_active,
                const gchar *counter,
                gboolean     counter_active)
{
    if (g_utf8_strlen (acc_label, -1) == 0 || g_utf8_strlen (secret, -1) == 0) {
        show_message_dialog (dialog, "Label and/or secret can't be empty", GTK_MESSAGE_ERROR);
        return FALSE;
    }
    if (!g_str_is_ascii (acc_label) || !g_str_is_ascii (acc_iss)) {
        gchar *msg = g_strconcat ("Only ASCII characters are supported. Entry with label '", acc_label, "' will not be added.", NULL);
        show_message_dialog (dialog, msg, GTK_MESSAGE_ERROR);
        g_free (msg);
        return FALSE;
    }
    if (!str_is_only_num_or_alpha (secret)) {
        gchar *msg = g_strconcat ("Secret can contain only characters from the english alphabet and digits. Entry with label '",
                                  acc_label, "' will not be added.", NULL);
        show_message_dialog (dialog, msg, GTK_MESSAGE_ERROR);
        g_free (msg);
        return FALSE;
    }
    if (!str_is_only_num (digits) || g_ascii_strtoll (digits, NULL, 10) < 4 || g_ascii_strtoll (digits, NULL, 10) > 10) {
        gchar *msg = g_strconcat ("The digits entry should contain only digits and the value should be between 4 and 10 inclusive.\n"
                                  "Entry with label '", acc_label, "' will not be added.", NULL);
        show_message_dialog (dialog, msg, GTK_MESSAGE_ERROR);
        g_free (msg);
        return FALSE;
    }
    if (period_active && (!str_is_only_num (period) || g_ascii_strtoll (period, NULL, 10) < 10 || g_ascii_strtoll (period, NULL, 10) > 120)) {
        gchar *msg = g_strconcat ("The period entry should contain only digits and the value should be between 10 and 120 (inclusive).\n"
                                  "Entry with label '", acc_label, "' will not be added.", NULL);
        show_message_dialog (dialog, msg, GTK_MESSAGE_ERROR);
        g_free (msg);
        return FALSE;
    }
    if (counter_active && (!str_is_only_num (counter) || g_ascii_strtoll (counter, NULL, 10) < 1 || g_ascii_strtoll (counter, NULL, 10) == G_MAXINT64)) {
        gchar *msg = g_strconcat ("The counter entry should contain only digits and the value should be between 1 and G_MAXINT64-1 (inclusive).\n"
                                  "Entry with label '", acc_label, "' will not be added.", NULL);
        show_message_dialog (dialog, msg, GTK_MESSAGE_ERROR);
        g_free (msg);
        return FALSE;
    }

    return TRUE;
}


static gboolean
str_is_only_num_or_alpha (const gchar *string)
{
    for (gint i = 0; i < strlen (string); i++) {
        if (!g_ascii_isalnum (string[i])) {
            return FALSE;
        }
    }
    return TRUE;
}


static gboolean
str_is_only_num (const gchar *string)
{
    for (gint i = 0; i < strlen (string); i++) {
        if (!g_ascii_isdigit (string[i])) {
            return FALSE;
        }
    }
    return TRUE;
}


static json_t *
get_json_obj (Widgets     *widgets,
              const gchar *acc_label,
              const gchar *acc_iss,
              const gchar *acc_key,
              const gchar *digits,
              const gchar *period,
              const gchar *counter)
{
    gchar *type = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (widgets->otp_cb));
    gchar *algo = gtk_combo_box_text_get_active_text (GTK_COMBO_BOX_TEXT (widgets->algo_cb));
    gint digits_int = (gint)g_ascii_strtoll (digits, NULL, 10);
    gint period_int = (gint)g_ascii_strtoll (period, NULL, 10);
    gint64 ctr = g_ascii_strtoll (counter, NULL, 10);
    json_t *jn = build_json_obj (type, acc_label, acc_iss, acc_key, digits_int, algo, period_int, ctr);
    g_free (type);
    g_free (algo);

    return jn;
}