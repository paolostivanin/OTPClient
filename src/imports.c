#include <gtk/gtk.h>
#include <gcrypt.h>
#include <jansson.h>
#include "app.h"
#include "db-misc.h"
#include "imports.h"
#include "password-cb.h"
#include "message-dialogs.h"
#include "gquarks.h"
#include "common.h"


static gboolean  parse_data_and_update_db    (AppData       *app_data,
                                              const gchar   *filename,
                                              const gchar   *action_name);


void
select_file_cb (GSimpleAction *simple,
                GVariant      *parameter __attribute__((unused)),
                gpointer       user_data)
{
    const gchar *action_name = g_action_get_name (G_ACTION (simple));
    AppData *app_data = (AppData *)user_data;
    GtkListStore *list_store = g_object_get_data (G_OBJECT (app_data->main_window), "lstore");

    GtkWidget *dialog = gtk_file_chooser_dialog_new ("Open File",
                                                     GTK_WINDOW (app_data->main_window),
                                                     GTK_FILE_CHOOSER_ACTION_OPEN,
                                                     "Cancel", GTK_RESPONSE_CANCEL,
                                                     "Open", GTK_RESPONSE_ACCEPT,
                                                     NULL);

#ifdef USE_FLATPAK_APP_FOLDER
    gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (dialog), g_get_user_data_dir ());
#endif

    gint res = gtk_dialog_run (GTK_DIALOG (dialog));
    if (res == GTK_RESPONSE_ACCEPT) {
        GtkFileChooser *chooser = GTK_FILE_CHOOSER (dialog);
        gchar *filename = gtk_file_chooser_get_filename (chooser);
        parse_data_and_update_db (app_data, filename, action_name);
        g_free (filename);
    }

    gtk_widget_destroy (dialog);
}


gchar *
update_db_from_otps (GSList *otps, DatabaseData *db_data, GtkListStore *list_store)
{
    json_t *obj;
    guint list_len = g_slist_length (otps);
    for (guint i = 0; i < list_len; i++) {
        otp_t *otp = g_slist_nth_data (otps, i);
        obj = build_json_obj (otp->type, otp->label, otp->issuer, otp->secret, otp->digits, otp->algo, otp->period, otp->counter);
        guint hash = json_object_get_hash (obj);
        if (g_slist_find_custom (db_data->objects_hash, GUINT_TO_POINTER (hash), check_duplicate) == NULL) {
            db_data->objects_hash = g_slist_append (db_data->objects_hash, g_memdup (&hash, sizeof (guint)));
            db_data->data_to_add = g_slist_append (db_data->data_to_add, obj);
        } else {
            g_print ("[INFO] Duplicate element not added\n");
        }
    }

    GError *err = NULL;
    update_and_reload_db (db_data, list_store, TRUE, &err);
    if (err != NULL && !g_error_matches (err, missing_file_gquark (), MISSING_FILE_CODE)) {
        return g_strdup (err->message);
    }

    return NULL;
}


void
free_otps_gslist (GSList *otps,
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


static gboolean
parse_data_and_update_db (AppData       *app_data,
                          const gchar   *filename,
                          const gchar   *action_name)
{
    GError *err = NULL;
    GSList *content = NULL;
    gchar *pwd = prompt_for_password (app_data->main_window, TRUE, NULL);
    if (pwd == NULL) {
        return FALSE;
    }

    if (g_strcmp0 (action_name, ANDOTP_IMPORT_ACTION_NAME) == 0) {
        content = get_andotp_data (filename, pwd, app_data->db_data->max_file_size_from_memlock, &err);
    } else {
        content = get_authplus_data (filename, pwd, app_data->db_data->max_file_size_from_memlock, &err);
    }

    if (content == NULL && err != NULL) {
        gchar *msg = g_strconcat ("An error occurred while importing, so nothing has been added to the database.\n"
                                  "The error is: ", err->message, NULL);
        show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
        g_free (msg);
        return FALSE;
    }

    gchar *err_msg = update_db_from_otps (content, app_data->db_data, GTK_LIST_STORE(gtk_tree_view_get_model (app_data->tree_view)));
    if (err_msg != NULL) {
        show_message_dialog (app_data->main_window, err_msg, GTK_MESSAGE_ERROR);
        g_free (err_msg);
        return FALSE;
    }

    gcry_free (pwd);
    free_otps_gslist (content, g_slist_length (content));

    return TRUE;
}
