#include <gtk/gtk.h>
#include <gcrypt.h>
#include "imports.h"
#include "qrcode-parser.h"
#include "message-dialogs.h"
#include "add-common.h"

static void
parse_file_and_update_db (const gchar *filename,
                          ImportData  *import_data);


void
select_photo_cb (GSimpleAction *simple    __attribute__((unused)),
                 GVariant      *parameter __attribute__((unused)),
                 gpointer       user_data)
{
    ImportData *import_data = (ImportData *)user_data;

    GtkWidget *dialog = gtk_file_chooser_dialog_new ("Open File",
                                                     GTK_WINDOW (import_data->main_window),
                                                     GTK_FILE_CHOOSER_ACTION_OPEN,
                                                     "Cancel", GTK_RESPONSE_CANCEL,
                                                     "Open", GTK_RESPONSE_ACCEPT,
                                                     NULL);

    GtkFileFilter *filter = gtk_file_filter_new ();
    gtk_file_filter_add_pattern (filter, "*.png");
    gtk_file_chooser_add_filter (GTK_FILE_CHOOSER (dialog), filter);

#ifdef USE_FLATPAK_APP_FOLDER
    gtk_file_chooser_set_current_folder (GTK_FILE_CHOOSER (dialog), g_get_user_data_dir ());
#endif

    gint res = gtk_dialog_run (GTK_DIALOG (dialog));
    if (res == GTK_RESPONSE_ACCEPT) {
        gchar *filename = gtk_file_chooser_get_filename (GTK_FILE_CHOOSER (dialog));
        parse_file_and_update_db (filename, import_data);
        g_free (filename);
    }
    gtk_widget_destroy (dialog);
}


static void
parse_file_and_update_db (const gchar *filename,
                          ImportData  *import_data)
{
    gchar *otpauth_uri = NULL;
    gchar *err_msg = parse_qrcode (filename, &otpauth_uri);
    if (err_msg != NULL) {
        show_message_dialog(import_data->main_window, err_msg, GTK_MESSAGE_ERROR);
        g_free(err_msg);
        return;
    }

    err_msg = add_data_to_db (otpauth_uri, import_data);
    if (err_msg != NULL) {
        show_message_dialog (import_data->main_window, err_msg, GTK_MESSAGE_ERROR);
        g_free (err_msg);
    } else {
        show_message_dialog (import_data->main_window, "QRCode successfully imported from the screenshot", GTK_MESSAGE_INFO);
    }
    gcry_free (otpauth_uri);
}