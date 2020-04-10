#include <gtk/gtk.h>
#include <gcrypt.h>
#include <jansson.h>
#include "imports.h"
#include "password-cb.h"
#include "message-dialogs.h"
#include "gquarks.h"
#include "common/common.h"
#include "gui-common.h"
#include "db-misc.h"


static gboolean  parse_data_and_update_db    (AppData       *app_data,
                                              const gchar   *filename,
                                              const gchar   *action_name);


void
select_file_cb (GSimpleAction *simple,
                GVariant      *parameter __attribute__((unused)),
                gpointer       user_data)
{
    const gchar *action_name = g_action_get_name (G_ACTION(simple));
    AppData *app_data = (AppData *)user_data;

#if GTK_CHECK_VERSION(3, 20, 0)
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new ("Open File",
                                                     GTK_WINDOW(app_data->main_window),
                                                     GTK_FILE_CHOOSER_ACTION_OPEN,
                                                     "Open",
                                                     "Cancel");
    gint res = gtk_native_dialog_run (GTK_NATIVE_DIALOG(dialog));
#else
    GtkWidget *dialog = gtk_file_chooser_dialog_new ("Open File",
                                                     GTK_WINDOW(app_data->main_window),
                                                     GTK_FILE_CHOOSER_ACTION_OPEN,
                                                     "Cancel", GTK_RESPONSE_CANCEL,
                                                     "Open", GTK_RESPONSE_ACCEPT,
                                                     NULL);
    gint res = gtk_dialog_run (GTK_DIALOG(dialog));
#endif
    if (res == GTK_RESPONSE_ACCEPT) {
        GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
        gchar *filename = gtk_file_chooser_get_filename (chooser);
        parse_data_and_update_db (app_data, filename, action_name);
        g_free (filename);
    }
#if GTK_CHECK_VERSION(3, 20, 0)
    g_object_unref (dialog);
#else
    gtk_widget_destroy (dialog);
#endif
}


gchar *
update_db_from_otps (GSList *otps, AppData *app_data)
{
    json_t *obj;
    guint list_len = g_slist_length (otps);
    for (guint i = 0; i < list_len; i++) {
        otp_t *otp = g_slist_nth_data (otps, i);
        obj = build_json_obj (otp->type, otp->account_name, otp->issuer, otp->secret, otp->digits, otp->algo, otp->period, otp->counter);
        guint hash = json_object_get_hash (obj);
        if (g_slist_find_custom (app_data->db_data->objects_hash, GUINT_TO_POINTER(hash), check_duplicate) == NULL) {
            app_data->db_data->objects_hash = g_slist_append (app_data->db_data->objects_hash, g_memdup (&hash, sizeof (guint)));
            app_data->db_data->data_to_add = g_slist_append (app_data->db_data->data_to_add, obj);
        } else {
            g_print ("[INFO] Duplicate element not added\n");
        }
    }

    GError *err = NULL;
    update_and_reload_db (app_data, app_data->db_data, TRUE, &err);
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
        g_free (otp_data->account_name);
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

    gchar *pwd = NULL;
    if (g_strcmp0 (action_name, ANDOTP_IMPORT_ACTION_NAME) == 0) {
        pwd = prompt_for_password (app_data, NULL, action_name, FALSE);
        if (pwd == NULL) {
            return FALSE;
        }
    }

    if (g_strcmp0 (action_name, ANDOTP_IMPORT_ACTION_NAME) == 0 || g_strcmp0 (action_name, ANDOTP_IMPORT_PLAIN_ACTION_NAME) == 0) {
        content = get_andotp_data (filename, pwd, app_data->db_data->max_file_size_from_memlock, g_strcmp0 (action_name, ANDOTP_IMPORT_ACTION_NAME) == 0 ? TRUE : FALSE , &err);
    } else if (g_strcmp0 (action_name, AUTHPLUS_IMPORT_ACTION_NAME) == 0) {
        content = get_authplus_data (filename, pwd, app_data->db_data->max_file_size_from_memlock, &err);
    } else if (g_strcmp0 (action_name, FREEOTPPLUS_IMPORT_ACTION_NAME) == 0) {
        content = get_freeotpplus_data (filename, &err);
    }

    if (content == NULL && err != NULL) {
        gchar *msg = g_strconcat ("An error occurred while importing, so nothing has been added to the database.\n"
                                  "The error is: ", err->message, NULL);
        show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
        g_free (msg);
        return FALSE;
    }

    gchar *err_msg = update_db_from_otps (content, app_data);
    if (err_msg != NULL) {
        show_message_dialog (app_data->main_window, err_msg, GTK_MESSAGE_ERROR);
        g_free (err_msg);
        return FALSE;
    }

    if (pwd != NULL) {
        gcry_free (pwd);
    }
    free_otps_gslist (content, g_slist_length (content));

    return TRUE;
}
