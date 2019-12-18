#include <gtk/gtk.h>
#include <gcrypt.h>
#include "gui-common.h"
#include "message-dialogs.h"
#include "get-builder.h"
#include "otpclient.h"

typedef struct _entrywidgets {
    GtkWidget *entry_old;
    GtkWidget *entry1;
    GtkWidget *entry2;
    gboolean retry;
    gchar *pwd;
    gchar *cur_pwd;
} EntryWidgets;

static void check_pwd_cb  (GtkWidget *entry,
                           gpointer   user_data);

static void password_cb   (GtkWidget *entry,
                           gpointer  *pwd);


gchar *
prompt_for_password (AppData        *app_data,
                     gchar          *current_key,
                     const gchar    *action_name,
                     gboolean        is_export_pwd)
{
    EntryWidgets *entry_widgets = g_new0 (EntryWidgets, 1);
    entry_widgets->retry = FALSE;

    GtkBuilder *builder = get_builder_from_partial_path (UI_PARTIAL_PATH);
    GtkWidget *dialog;

    gboolean pwd_must_be_checked = TRUE;
    gboolean file_exists = g_file_test (app_data->db_data->db_path, G_FILE_TEST_EXISTS);
    if ((file_exists == TRUE || action_name != NULL) && current_key == NULL && is_export_pwd == FALSE) {
        // decrypt dialog, just one field
        pwd_must_be_checked = FALSE;
        dialog = GTK_WIDGET(gtk_builder_get_object (builder, "decpwd_diag_id"));
        gchar *text = NULL, *markup = NULL;
        if (action_name == NULL){
            markup = g_markup_printf_escaped ("%s <span font_family=\"monospace\">%s</span>", "Enter the decryption password for\n", app_data->db_data->db_path);
        } else {
            text = g_strdup ("Enter the decryption password");
        }
        GtkLabel *label = GTK_LABEL(gtk_builder_get_object (builder, "decpwd_label_id"));
        if (markup != NULL) {
            gtk_label_set_markup (label, markup);
            g_free (markup);
        } else {
            gtk_label_set_text (label, text);
            g_free (text);
        }
        entry_widgets->entry1 = GTK_WIDGET(gtk_builder_get_object (builder,"decpwddiag_entry_id"));
        g_signal_connect (entry_widgets->entry1, "activate", G_CALLBACK (send_ok_cb), NULL);
        g_signal_connect (entry_widgets->entry1, "icon-press", G_CALLBACK (icon_press_cb), NULL);
    } else if ((file_exists == FALSE && current_key == NULL) || is_export_pwd == TRUE) {
        // new db dialog, 2 fields
        dialog = GTK_WIDGET(gtk_builder_get_object (builder, "newdb_pwd_diag_id"));
        entry_widgets->entry1 = GTK_WIDGET(gtk_builder_get_object (builder,"newdb_pwd_diag_entry1_id"));
        entry_widgets->entry2 = GTK_WIDGET(gtk_builder_get_object (builder,"newdb_pwd_diag_entry2_id"));
        g_signal_connect (entry_widgets->entry2, "activate", G_CALLBACK (send_ok_cb), NULL);
        g_signal_connect (entry_widgets->entry1, "icon-press", G_CALLBACK (icon_press_cb), NULL);
        g_signal_connect (entry_widgets->entry2, "icon-press", G_CALLBACK (icon_press_cb), NULL);
    } else {
        // change pwd dialog, 3 fields
        if (current_key == NULL) {
            show_message_dialog (app_data->main_window, "ERROR: current_key cannot be NULL", GTK_MESSAGE_ERROR);
            g_free (entry_widgets);
            g_object_unref (builder);
            return NULL;
        }
        dialog = GTK_WIDGET(gtk_builder_get_object (builder, "changepwd_diag_id"));
        entry_widgets->cur_pwd = secure_strdup (current_key);
        entry_widgets->entry_old = GTK_WIDGET(gtk_builder_get_object (builder,"changepwd_diag_currententry_id"));
        entry_widgets->entry1 = GTK_WIDGET(gtk_builder_get_object (builder,"changepwd_diag_newentry1_id"));
        entry_widgets->entry2 = GTK_WIDGET(gtk_builder_get_object (builder,"changepwd_diag_newentry2_id"));
        g_signal_connect (entry_widgets->entry2, "activate", G_CALLBACK (send_ok_cb), NULL);
        g_signal_connect (entry_widgets->entry1, "icon-press", G_CALLBACK (icon_press_cb), NULL);
        g_signal_connect (entry_widgets->entry2, "icon-press", G_CALLBACK (icon_press_cb), NULL);
        g_signal_connect (entry_widgets->entry_old, "icon-press", G_CALLBACK (icon_press_cb), NULL);
    }

    gtk_window_set_transient_for (GTK_WINDOW(dialog), GTK_WINDOW(app_data->main_window));

    gtk_widget_show_all (dialog);

    gint ret;
    do {
        ret = gtk_dialog_run (GTK_DIALOG(dialog));
        if (ret == GTK_RESPONSE_OK) {
            if ((file_exists == TRUE || action_name != NULL) && pwd_must_be_checked == FALSE) {
                password_cb (entry_widgets->entry1, (gpointer *)&entry_widgets->pwd);
            } else {
                check_pwd_cb (entry_widgets->entry1, (gpointer)entry_widgets);
            }
        }
    } while (ret == GTK_RESPONSE_OK && entry_widgets->retry == TRUE);

    gchar *pwd = NULL;
    if (entry_widgets->pwd != NULL) {
        gcry_free (current_key);
        gsize len = strlen (entry_widgets->pwd) + 1;
        pwd = gcry_calloc_secure (len, 1);
        strncpy (pwd, entry_widgets->pwd, len);
        gcry_free (entry_widgets->pwd);
    }
    if (entry_widgets->cur_pwd != NULL) {
        gcry_free (entry_widgets->cur_pwd);
    }

    g_free (entry_widgets);

    gtk_widget_destroy (dialog);

    g_object_unref (builder);

    return pwd;
}


static void
check_pwd_cb (GtkWidget   *entry,
              gpointer     user_data)
{
    EntryWidgets *entry_widgets = (EntryWidgets *) user_data;
    if (entry_widgets->cur_pwd != NULL && g_strcmp0 (gtk_entry_get_text (GTK_ENTRY(entry_widgets->entry_old)), entry_widgets->cur_pwd) != 0) {
        show_message_dialog (gtk_widget_get_toplevel (entry), "Old password doesn't match", GTK_MESSAGE_ERROR);
        entry_widgets->retry = TRUE;
        return;
    }
    if (gtk_entry_get_text_length (GTK_ENTRY(entry_widgets->entry1)) < 6) {
        show_message_dialog (gtk_widget_get_toplevel (entry), "Password must be at least 6 characters.", GTK_MESSAGE_ERROR);
        entry_widgets->retry = TRUE;
        return;
    }
    if (g_strcmp0 (gtk_entry_get_text (GTK_ENTRY(entry_widgets->entry1)), gtk_entry_get_text (GTK_ENTRY(entry_widgets->entry2))) == 0) {
        password_cb (entry, (gpointer *)&entry_widgets->pwd);
        entry_widgets->retry = FALSE;
    } else {
        show_message_dialog (gtk_widget_get_toplevel (entry), "Passwords mismatch", GTK_MESSAGE_ERROR);
        entry_widgets->retry = TRUE;
    }
}


static void
password_cb (GtkWidget  *entry,
             gpointer   *pwd)
{
    const gchar *text = gtk_entry_get_text (GTK_ENTRY(entry));
    gsize len = strlen (text) + 1;
    *pwd = gcry_calloc_secure (len, 1);
    strncpy (*pwd, text, len);
    GtkWidget *top_level = gtk_widget_get_toplevel (entry);
    gtk_dialog_response (GTK_DIALOG (top_level), GTK_RESPONSE_CLOSE);
}
