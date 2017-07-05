#include <gtk/gtk.h>
#include <gcrypt.h>
#include "otpclient.h"
#include "kf-misc.h"

static GtkWidget *create_main_window (GtkApplication *app, GdkPixbuf *logo);

static gchar *prompt_for_password (GtkWidget *main_window);

static void icon_press_cb (GtkEntry *entry, gint position, GdkEventButton *, gpointer);


void
activate (GtkApplication *app, gpointer user_data)
{
    GdkPixbuf *logo = (GdkPixbuf *)user_data;
    GtkWidget *main_window = create_main_window (app, logo);
    gtk_application_add_window (GTK_APPLICATION (app), GTK_WINDOW (main_window));

    if (!gcry_check_version ("1.6.0")) {
        show_message_dialog (main_window, "The required version of GCrypt is 1.6.0 or greater.", GTK_MESSAGE_ERROR);
        return;
    }

    gcry_control (GCRYCTL_INIT_SECMEM, 16384, 0);
    gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);

    gchar *pwd = prompt_for_password (main_window);
    gchar *dec_kf = load_kf (pwd);
    if (dec_kf == FILE_EMPTY) {
        show_message_dialog (main_window, "There is no data inside the file.\n", GTK_MESSAGE_INFO);
    }

    create_scrolled_window_with_treeview (main_window, dec_kf, pwd);

    // TODO do the following before exiting
    gcry_free (dec_kf);
    gcry_free (pwd);

    gtk_widget_show_all (main_window);
}


static GtkWidget *
create_main_window (GtkApplication *app, GdkPixbuf *logo)
{
    // TODO add gtk label with countdown
    GtkWidget *window = gtk_application_window_new (app);
    gtk_window_set_position (GTK_WINDOW (window), GTK_WIN_POS_CENTER);
    gtk_window_set_resizable (GTK_WINDOW (window), FALSE);

    if (logo != NULL)
        gtk_window_set_icon (GTK_WINDOW (window), logo);

    gtk_container_set_border_width (GTK_CONTAINER (window), 10);

    gtk_widget_set_size_request (GTK_WIDGET (window), 475, 360);

    gchar *header_bar_text = g_malloc (strlen (APP_NAME ) + 1 + strlen (APP_VERSION) + 1);
    g_snprintf (header_bar_text, strlen (APP_NAME) + 1 + strlen (APP_VERSION) + 1, "%s %s", APP_NAME, APP_VERSION);
    header_bar_text[strlen(header_bar_text)] = '\0';

    GtkWidget *header_bar = gtk_header_bar_new ();
    gtk_header_bar_set_show_close_button (GTK_HEADER_BAR (header_bar), TRUE);
    gtk_header_bar_set_title (GTK_HEADER_BAR (header_bar), header_bar_text);
    gtk_header_bar_set_has_subtitle (GTK_HEADER_BAR (header_bar), FALSE);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_style_context_add_class (gtk_widget_get_style_context (box), "linked");

    gtk_window_set_titlebar (GTK_WINDOW (window), header_bar);

    return window;
}


static gchar *
prompt_for_password (GtkWidget *mw)
{
    GtkDialogFlags flags = GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT;
    GtkWidget *dialog = gtk_dialog_new_with_buttons ("Password", GTK_WINDOW (mw), flags, "OK", GTK_RESPONSE_ACCEPT,
                                                     "Cancel", GTK_RESPONSE_CLOSE, NULL);

    gtk_window_set_position (GTK_WINDOW (dialog), GTK_WIN_POS_CENTER);

    GIcon *gicon = g_themed_icon_new_with_default_fallbacks ("dialog-password-symbolic");

    GtkWidget *entry = gtk_entry_new ();
    gtk_entry_set_icon_from_gicon (GTK_ENTRY (entry), GTK_ENTRY_ICON_SECONDARY, gicon);
    gtk_entry_set_icon_activatable (GTK_ENTRY (entry), GTK_ENTRY_ICON_SECONDARY, TRUE);
    gtk_entry_set_icon_tooltip_text (GTK_ENTRY (entry), GTK_ENTRY_ICON_SECONDARY, "Show password");
    gtk_entry_set_visibility (GTK_ENTRY (entry), FALSE);
    g_signal_connect(entry, "icon-press", G_CALLBACK (icon_press_cb), NULL);

    GtkWidget *content_area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    gtk_container_add (GTK_CONTAINER (content_area), entry);

    gtk_widget_show_all (dialog);

    gchar *pwd = NULL;
    const gchar *text = NULL;
    gint ret = gtk_dialog_run (GTK_DIALOG (dialog));
    switch (ret) {
        case GTK_RESPONSE_ACCEPT:
            text = gtk_entry_get_text (GTK_ENTRY (entry));
            pwd = gcry_malloc_secure (strlen (text) + 1);
            strncpy (pwd, text, strlen (text) + 1);
            break;
        case GTK_RESPONSE_CLOSE:
            break;
        default:
            break;
    }
    gtk_widget_destroy (dialog);

    return pwd;
}


static void
icon_press_cb (GtkEntry *entry, gint position, GdkEventButton *event, gpointer data)
{
    gtk_entry_set_visibility (GTK_ENTRY (entry), !gtk_entry_get_visibility (entry));
}
