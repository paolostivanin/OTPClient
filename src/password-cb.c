#include <gtk/gtk.h>
#include <gcrypt.h>
#include "common.h"
#include "message-dialogs.h"

typedef struct _entrywidgets {
    GtkWidget *entry_old;
    GtkWidget *entry1;
    GtkWidget *entry2;
    gboolean retry;
    gchar *pwd;
    gchar *cur_pwd;
} EntryWidgets;

static GtkWidget *get_vbox      (EntryWidgets *entry_widgets,
                                 gboolean      file_exists,
                                 const gchar  *current_key);

static void       check_pwd_cb  (GtkWidget *entry,
                                 gpointer   user_data);

static void       password_cb   (GtkWidget *entry,
                                 gpointer  *pwd);


gchar *
prompt_for_password (GtkWidget *main_window, gboolean file_exists, gchar *current_key)
{
    GtkDialogFlags flags = GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT;
    GtkWidget *dialog = gtk_dialog_new_with_buttons ("Password",
                                                     GTK_WINDOW (main_window),
                                                     flags,
                                                     "Cancel", GTK_RESPONSE_CLOSE,
                                                     "OK", GTK_RESPONSE_ACCEPT,
                                                     NULL);


    gtk_window_set_position (GTK_WINDOW (dialog), GTK_WIN_POS_CENTER);

    EntryWidgets *entry_widgets = g_new0 (EntryWidgets, 1);
    entry_widgets->retry = FALSE;

    GtkWidget *vbox = get_vbox (entry_widgets, file_exists, current_key);
    gtk_widget_set_margin_bottom (vbox, 10);

    GtkWidget *content_area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    gtk_container_add (GTK_CONTAINER (content_area), vbox);

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
    g_free (entry_widgets);

    gtk_widget_destroy (dialog);

    return pwd;
}


static GtkWidget *
get_vbox (EntryWidgets *entry_widgets,
          gboolean      file_exists,
          const gchar  *current_key)
{
    GtkWidget *vbox = gtk_box_new (GTK_ORIENTATION_VERTICAL, 3);

    if (current_key != NULL) {
        entry_widgets->entry_old = gtk_entry_new ();
        gtk_entry_set_placeholder_text (GTK_ENTRY (entry_widgets->entry_old), "Type current password...");
        gtk_widget_set_tooltip_text (entry_widgets->entry_old, "Current password");
        set_icon_to_entry (entry_widgets->entry_old, "dialog-password-symbolic", "Show password");
        entry_widgets->cur_pwd = (gchar *)current_key;
    }

    entry_widgets->entry1 = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (entry_widgets->entry1), "Type password...");
    set_icon_to_entry (entry_widgets->entry1, "dialog-password-symbolic", "Show password");

    GtkWidget *label = gtk_label_new (NULL);
    gtk_label_set_use_markup (GTK_LABEL (label), TRUE);
    gtk_label_set_justify (GTK_LABEL (label), GTK_JUSTIFY_CENTER);
    gtk_widget_set_margin_bottom (label, 5);

    gtk_box_pack_start (GTK_BOX (vbox), label, TRUE, TRUE, 0);
    if (current_key != NULL) {
        gtk_box_pack_start (GTK_BOX (vbox), entry_widgets->entry_old, TRUE, TRUE, 0);
    }
    gtk_box_pack_start (GTK_BOX (vbox), entry_widgets->entry1, TRUE, TRUE, 0);

    const gchar *str;
    if (!file_exists) {
        entry_widgets->entry2 = gtk_entry_new ();
        gtk_entry_set_placeholder_text (GTK_ENTRY (entry_widgets->entry2), "Retype password...");
        set_icon_to_entry (entry_widgets->entry2, "dialog-password-symbolic", "Show password");

        g_signal_connect (entry_widgets->entry2, "activate", G_CALLBACK (check_pwd_cb), entry_widgets);

        str = "Choose an encryption password for the database.\n"
                "Please note that if the password is lost or forgotten, <b>there will be no way to recover it</b>.";

        gtk_box_pack_end (GTK_BOX (vbox), entry_widgets->entry2, TRUE, TRUE, 0);
    } else {
        str = "Enter the decryption password.";
        g_signal_connect (entry_widgets->entry1, "activate", G_CALLBACK (password_cb), (gpointer *) &entry_widgets->pwd);
    }

    gtk_label_set_label (GTK_LABEL (label), str);

    return vbox;
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
