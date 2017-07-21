#include <gtk/gtk.h>
#include <gcrypt.h>
#include "otpclient.h"
#include "treeview.h"
#include "common.h"

static GtkWidget *create_main_window_with_header_bar (GtkApplication *app, GdkPixbuf *logo);

static gchar *prompt_for_password (GtkWidget *main_window);

static void add_data_cb (GtkWidget *widget, gpointer user_data);

static void del_data_cb (GtkWidget *widget, gpointer user_data);

static void destroy_cb (GtkWidget *window, gpointer user_data);


void
activate (GtkApplication *app, gpointer user_data)
{
    GdkPixbuf *logo = (GdkPixbuf *)user_data;
    GtkWidget *main_window = create_main_window_with_header_bar (app, logo);
    gtk_application_add_window (GTK_APPLICATION (app), GTK_WINDOW (main_window));

    if (!gcry_check_version ("1.6.0")) {
        show_message_dialog (main_window, "The required version of GCrypt is 1.6.0 or greater.", GTK_MESSAGE_ERROR);
        return;
    }

    gcry_control (GCRYCTL_INIT_SECMEM, 16384, 0);
    gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);

    UpdateData *kf_update_data = g_new0 (UpdateData, 1);
    kf_update_data->key = prompt_for_password (main_window);
    kf_update_data->in_memory_kf = load_kf (kf_update_data->key);

    g_signal_connect (find_widget (main_window, "add_btn_app"), "clicked", G_CALLBACK (add_data_cb), kf_update_data);
    g_signal_connect (find_widget (main_window, "del_btn_app"), "clicked", G_CALLBACK (del_data_cb), kf_update_data);
    g_signal_connect (main_window, "destroy", G_CALLBACK (destroy_cb), kf_update_data);

    create_scrolled_window_with_treeview (main_window, kf_update_data);

    gtk_widget_show_all (main_window);
}


static GtkWidget *
create_main_window_with_header_bar (GtkApplication *app, GdkPixbuf *logo)
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

    GtkWidget *header_bar = create_header_bar (header_bar_text);
    GtkWidget *box = create_box_with_buttons ("add_btn_app", "del_btn_app");
    gtk_header_bar_pack_start (GTK_HEADER_BAR (header_bar), box);
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

    GtkWidget *entry = gtk_entry_new ();

    set_icon_to_entry (entry, "dialog-password-symbolic", "Show password");

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
add_data_cb (GtkWidget *btn,
             gpointer user_data)
{
    GtkWidget *top_level = gtk_widget_get_toplevel (btn);
    UpdateData *kf_data = (UpdateData *)user_data;
    add_data_dialog (top_level, kf_data);
}


static void
del_data_cb (GtkWidget *btn,
             gpointer user_data)
{
    GtkWidget *top_level = gtk_widget_get_toplevel (btn);
    UpdateData *kf_data = (UpdateData *)user_data;
    // TODO complete me
}


static void
destroy_cb (GtkWidget *win __attribute__((__unused__)),
            gpointer user_data)
{
    UpdateData *kf_update_data = user_data;
    gcry_free (kf_update_data->key);
    gcry_free (kf_update_data->in_memory_kf);
    g_free (kf_update_data);
}