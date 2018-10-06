#include <gtk/gtk.h>
#include <gcrypt.h>
#include "common.h"
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
prompt_for_password (const gchar *db_path, gchar *current_key)
{
    GtkBuilder *builder = get_builder_from_partial_path (UI_PARTIAL_PATH);

    EntryWidgets *entry_widgets = g_new0 (EntryWidgets, 1);
    entry_widgets->retry = FALSE;

    gboolean file_exists = g_file_test (db_path, G_FILE_TEST_EXISTS);
    GtkWidget *dialog;
    if (file_exists == TRUE && current_key == NULL) {
        // decrypt dialog, just one field
        dialog = GTK_WIDGET (gtk_builder_get_object (builder, "decpwd_diag_id"));
        gchar *text = g_strconcat ("Enter the decryption password for ", db_path, NULL);
        gtk_label_set_text (GTK_LABEL(gtk_builder_get_object (builder, "decpwd_label_id")), text);
        g_free (text);
        entry_widgets->entry1 = GTK_WIDGET (gtk_builder_get_object (builder,"decpwddiag_entry_id"));
        g_signal_connect (entry_widgets->entry1, "activate", G_CALLBACK (password_cb), (gpointer *) &entry_widgets->pwd);
        g_signal_connect (entry_widgets->entry1, "icon-press", G_CALLBACK (icon_press_cb), NULL);
    } else if (file_exists == FALSE && current_key == NULL) {
        // new db dialog, 2 fields
        dialog = GTK_WIDGET (gtk_builder_get_object (builder, "newdb_pwd_diag_id"));
        entry_widgets->entry1 = GTK_WIDGET (gtk_builder_get_object (builder,"newdb_pwd_diag_entry1_id"));
        entry_widgets->entry2 = GTK_WIDGET (gtk_builder_get_object (builder,"newdb_pwd_diag_entry2_id"));
        g_signal_connect (entry_widgets->entry2, "activate", G_CALLBACK (check_pwd_cb), entry_widgets);
        g_signal_connect (entry_widgets->entry1, "icon-press", G_CALLBACK (icon_press_cb), NULL);
        g_signal_connect (entry_widgets->entry2, "icon-press", G_CALLBACK (icon_press_cb), NULL);
    } else {
        // change pwd dialog, 3 fields
        dialog = GTK_WIDGET (gtk_builder_get_object (builder, "changepwd_diag_id"));
        entry_widgets->cur_pwd = secure_strdup (current_key);
        entry_widgets->entry_old = GTK_WIDGET (gtk_builder_get_object (builder,"changepwd_diag_currententry_id"));
        entry_widgets->entry1 = GTK_WIDGET (gtk_builder_get_object (builder,"changepwd_diag_newentry1_id"));
        entry_widgets->entry2 = GTK_WIDGET (gtk_builder_get_object (builder,"changepwd_diag_newentry2_id"));
        g_signal_connect (entry_widgets->entry2, "activate", G_CALLBACK (check_pwd_cb), entry_widgets);
        g_signal_connect (entry_widgets->entry1, "icon-press", G_CALLBACK (icon_press_cb), NULL);
        g_signal_connect (entry_widgets->entry2, "icon-press", G_CALLBACK (icon_press_cb), NULL);
        g_signal_connect (entry_widgets->entry_old, "icon-press", G_CALLBACK (icon_press_cb), NULL);
    }

    gtk_widget_show_all (dialog);

    gint ret;
    do {
        ret = gtk_dialog_run (GTK_DIALOG (dialog));
        if (ret == GTK_RESPONSE_ACCEPT) {
            if (file_exists) {
                password_cb (entry_widgets->entry1, (gpointer *) &entry_widgets->pwd);
            } else {
                check_pwd_cb (entry_widgets->entry1, (gpointer) entry_widgets);
            }
        }
    } while (ret == GTK_RESPONSE_ACCEPT && entry_widgets->retry == TRUE);

    gchar *pwd = NULL;
    if (entry_widgets->pwd != NULL) {
        gcry_free (current_key);
        pwd = gcry_calloc_secure (strlen (entry_widgets->pwd) + 1, 1);
        strncpy (pwd, entry_widgets->pwd, strlen (entry_widgets->pwd) + 1);
        gcry_free (entry_widgets->pwd);
    }
    if (entry_widgets->cur_pwd != NULL) {
        gcry_free (entry_widgets->cur_pwd);
    }

    g_free (entry_widgets);
    g_object_unref (builder);
    gtk_widget_destroy (dialog);

    return pwd;
}


static void
check_pwd_cb (GtkWidget   *entry,
              gpointer     user_data)
{
    EntryWidgets *entry_widgets = (EntryWidgets *) user_data;
    if (entry_widgets->cur_pwd != NULL && g_strcmp0 (gtk_entry_get_text (GTK_ENTRY (entry_widgets->entry_old)), entry_widgets->cur_pwd) != 0) {
        show_message_dialog (gtk_widget_get_toplevel (entry), "Old password doesn't match", GTK_MESSAGE_ERROR);
        entry_widgets->retry = TRUE;
        return;
    }
    if (gtk_entry_get_text_length (GTK_ENTRY (entry_widgets->entry1)) < 6) {
        show_message_dialog (gtk_widget_get_toplevel (entry), "Password must be at least 6 characters.", GTK_MESSAGE_ERROR);
        entry_widgets->retry = TRUE;
        return;
    }
    if (g_strcmp0 (gtk_entry_get_text (GTK_ENTRY (entry_widgets->entry1)), gtk_entry_get_text (GTK_ENTRY (entry_widgets->entry2))) == 0) {
        password_cb (entry, (gpointer *) &entry_widgets->pwd);
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
    const gchar *text = gtk_entry_get_text (GTK_ENTRY (entry));
    *pwd = gcry_calloc_secure (strlen (text) + 1, 1);
    strncpy (*pwd, text, strlen (text) + 1);
    GtkWidget *top_level = gtk_widget_get_toplevel (entry);
    gtk_dialog_response (GTK_DIALOG (top_level), GTK_RESPONSE_CLOSE);
}
