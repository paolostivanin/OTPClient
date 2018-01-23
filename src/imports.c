#include <gtk/gtk.h>
#include <gcrypt.h>
#include "db-misc.h"
#include "imports.h"
#include "password-cb.h"
#include "message-dialogs.h"
#include "gquarks.h"


static gboolean  parse_data_and_update_db    (GtkWidget     *main_window,
                                              const gchar   *filename,
                                              const gchar   *action_name,
                                              DatabaseData  *db_data,
                                              GtkListStore  *list_store);


static JsonNode *get_json_node               (otp_t         *otp);

static void      free_gslist                 (GSList        *otps,
                                              guint          list_len);


void
select_file_cb (GSimpleAction *simple,
                GVariant      *parameter __attribute__((unused)),
                gpointer       user_data)
{
    const gchar *action_name = g_action_get_name (G_ACTION (simple));
    ImportData *import_data = (ImportData *)user_data;
    GtkListStore *list_store = g_object_get_data (G_OBJECT (import_data->main_window), "lstore");

    GtkWidget *dialog = gtk_file_chooser_dialog_new ("Open File",
                                                     GTK_WINDOW (import_data->main_window),
                                                     GTK_FILE_CHOOSER_ACTION_OPEN,
                                                     "Cancel", GTK_RESPONSE_CANCEL,
                                                     "Open", GTK_RESPONSE_ACCEPT,
                                                     NULL);

    gint res = gtk_dialog_run (GTK_DIALOG (dialog));
    if (res == GTK_RESPONSE_ACCEPT) {
        GtkFileChooser *chooser = GTK_FILE_CHOOSER (dialog);
        gchar *filename = gtk_file_chooser_get_filename (chooser);
        parse_data_and_update_db (import_data->main_window, filename, action_name, import_data->db_data, list_store);
        g_free (filename);
    }

    gtk_widget_destroy (dialog);
}


static gboolean
parse_data_and_update_db (GtkWidget     *main_window,
                          const gchar   *filename,
                          const gchar   *action_name,
                          DatabaseData  *db_data,
                          GtkListStore  *list_store)
{
    GError *err = NULL;
    GSList *content = NULL;
    gchar *pwd = prompt_for_password (main_window, TRUE);
    if (pwd == NULL) {
        return FALSE;
    }

    if (g_strcmp0 (action_name, ANDOTP_IMPORT_ACTION_NAME) == 0) {
        content = get_andotp_data (filename, pwd, db_data->max_file_size_from_memlock, &err);
    } else {
        content = get_authplus_data (filename, pwd, db_data->max_file_size_from_memlock, &err);
    }

    if (content == NULL && err != NULL) {
        gchar *msg = g_strconcat ("An error occurred while importing, so nothing has been added to the database.\n"
                                          "The error is: ", err->message, NULL);
        show_message_dialog (main_window, msg, GTK_MESSAGE_ERROR);
        g_free (msg);
        return FALSE;
    }

    JsonNode *jn;
    guint list_len = g_slist_length (content);
    for (guint i = 0; i < list_len; i++) {
        jn = get_json_node (g_slist_nth_data (content, i));
        guint hash = json_object_hash (json_node_get_object (jn));
        if (g_slist_find_custom (db_data->objects_hash, GUINT_TO_POINTER (hash), check_duplicate) == NULL) {
            db_data->objects_hash = g_slist_append (db_data->objects_hash, g_memdup (&hash, sizeof (guint)));
            db_data->data_to_add = g_slist_append (db_data->data_to_add, jn);
        } else {
            g_print ("[INFO] Duplicate element not added\n");
        }
    }

    update_and_reload_db (db_data, list_store, TRUE, &err);
    if (err != NULL && !g_error_matches (err, missing_file_gquark (), MISSING_FILE_CODE)) {
        show_message_dialog (main_window, err->message, GTK_MESSAGE_ERROR);
    }

    gcry_free (pwd);
    free_gslist (content, list_len);

    return TRUE;
}


static JsonNode *
get_json_node (otp_t *otp)
{
    JsonBuilder *jb = json_builder_new ();

    jb = json_builder_begin_object (jb);
    jb = json_builder_set_member_name (jb, "type");
    jb = json_builder_add_string_value (jb, otp->type);
    jb = json_builder_set_member_name (jb, "label");
    jb = json_builder_add_string_value (jb, otp->label);
    jb = json_builder_set_member_name (jb, "issuer");
    jb = json_builder_add_string_value (jb, otp->issuer);
    jb = json_builder_set_member_name (jb, "secret");
    jb = json_builder_add_string_value (jb, otp->secret);
    jb = json_builder_set_member_name (jb, "digits");
    jb = json_builder_add_int_value (jb, otp->digits);
    jb = json_builder_set_member_name (jb, "algo");
    jb = json_builder_add_string_value (jb, otp->algo);
    if (g_strcmp0 (otp->type, "TOTP") == 0) {
        jb = json_builder_set_member_name (jb, "period");
        json_builder_add_int_value (jb, otp->period);
    } else {
        jb = json_builder_set_member_name (jb, "counter");
        json_builder_add_int_value (jb, otp->counter);
    }
    jb = json_builder_end_object (jb);

    JsonNode *jnode = json_builder_get_root (jb);
    g_object_unref (jb);

    return jnode;
}


static void
free_gslist (GSList *otps,
             guint   list_len)
{
    otp_t *otp_data;
    for (guint i = 0; i < list_len; i++) {
        otp_data = g_slist_nth_data (otps, i);
        g_free (otp_data->type);
        g_free (otp_data->algo);
        g_free (otp_data->label);
        g_free (otp_data->issuer);
        gcry_free (otp_data->secret);
    }

    g_slist_free_full (otps, g_free);
}