#include <gtk/gtk.h>
#include <gcrypt.h>
#include "otpclient.h"
#include "common.h"
#include "gquarks.h"

static GtkWidget *create_main_window_with_header_bar (GtkApplication *app, GdkPixbuf *logo);

static gchar *prompt_for_password (GtkWidget *main_window);

void password_cb (GtkWidget *entry, gpointer *pwd);

static void add_data_cb (GtkWidget *btn, gpointer user_data);

static void del_data_cb (GtkWidget *btn, gpointer user_data);

static void destroy_cb (GtkWidget *window, gpointer user_data);


void
activate (GtkApplication    *app,
          gpointer           user_data)
{
    GdkPixbuf *logo = (GdkPixbuf *)user_data;
    GtkWidget *main_window = create_main_window_with_header_bar (app, logo);
    gtk_application_add_window (GTK_APPLICATION (app), GTK_WINDOW (main_window));

    if (!gcry_check_version ("1.6.0")) {
        show_message_dialog (main_window, "The required version of GCrypt is 1.6.0 or greater.", GTK_MESSAGE_ERROR);
        return;
    }

    gcry_control (GCRYCTL_INIT_SECMEM, MAX_FILE_SIZE, 0);
    gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);

    DatabaseData *db_data = g_new0 (DatabaseData, 1);
    db_data->objects_hash = NULL;
    db_data->data_to_add = NULL;
    db_data->key = prompt_for_password (main_window);
    if (db_data->key == NULL) {
        gtk_application_remove_window (GTK_APPLICATION (app), GTK_WINDOW (main_window));  

    GError *err = NULL;
    load_db (db_data, &err);
    if (err != NULL && !g_error_matches (err, missing_file_gquark (), MISSING_FILE_CODE)) {
        show_message_dialog (main_window, err->message, GTK_MESSAGE_ERROR);
        gcry_free (db_data->key);
        g_free (db_data);
        return;
    }

    GtkListStore *list_store = create_treeview (main_window, db_data);
    g_object_set_data (G_OBJECT (main_window), "lstore", list_store);

    g_signal_connect (find_widget (main_window, "add_btn_app"), "clicked", G_CALLBACK (add_data_cb), db_data);
    g_signal_connect (find_widget (main_window, "del_btn_app"), "clicked", G_CALLBACK (del_data_cb), db_data);
    g_signal_connect (main_window, "destroy", G_CALLBACK (destroy_cb), db_data);

    gtk_widget_show_all (main_window);
}


static GtkWidget *
create_main_window_with_header_bar (GtkApplication  *app,
                                    GdkPixbuf       *logo)
{
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

    g_free (header_bar_text);

    return window;
}


static gchar *
prompt_for_password (GtkWidget *main_window)
{
    GtkDialogFlags flags = GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT;
    GtkWidget *dialog = gtk_dialog_new_with_buttons ("Password", GTK_WINDOW (main_window), flags, "OK", GTK_RESPONSE_ACCEPT,
                                                     "Cancel", GTK_RESPONSE_CLOSE, NULL);

    gtk_window_set_position (GTK_WINDOW (dialog), GTK_WIN_POS_CENTER);

    gchar *pwd = NULL;
    GtkWidget *entry = gtk_entry_new ();
    g_signal_connect (entry, "activate", G_CALLBACK (password_cb), &pwd);

    set_icon_to_entry (entry, "dialog-password-symbolic", "Show password");

    GtkWidget *content_area = gtk_dialog_get_content_area (GTK_DIALOG (dialog));
    gtk_container_add (GTK_CONTAINER (content_area), entry);

    gtk_widget_show_all (dialog);

    gint ret = gtk_dialog_run (GTK_DIALOG (dialog));
    switch (ret) {
        case GTK_RESPONSE_ACCEPT:
            password_cb (entry, (gpointer *)&pwd);
            break;
        case GTK_RESPONSE_CLOSE:
            break;
        default:
            break;
    }
    gtk_widget_destroy (dialog);

    return pwd;
}


void
password_cb (GtkWidget  *entry,
             gpointer   *pwd)
{
    const gchar *text = gtk_entry_get_text (GTK_ENTRY (entry));
    *pwd = gcry_malloc_secure (strlen (text) + 1);
    strncpy (*pwd, text, strlen (text) + 1);
    GtkWidget *top_level = gtk_widget_get_toplevel (entry);
    gtk_dialog_response (GTK_DIALOG (top_level), GTK_RESPONSE_CLOSE);
}


static void
add_data_cb (GtkWidget *btn,
             gpointer   user_data)
{
    GtkWidget *top_level = gtk_widget_get_toplevel (btn);
    DatabaseData *db_data = (DatabaseData *)user_data;
    GtkListStore *list_store = g_object_get_data (G_OBJECT (top_level), "lstore");
    add_data_dialog (top_level, db_data, list_store);
}


static void
del_data_cb (GtkWidget *btn __attribute__((__unused__)),
             gpointer   user_data __attribute__((__unused__)))
{
    // TODO complete me
    //GtkWidget *top_level = gtk_widget_get_toplevel (btn);
    //DatabaseData *db_data = (DatabaseData *) user_data;
}


static void
destroy_cb (GtkWidget   *window __attribute__((__unused__)),
            gpointer     user_data)
{
    DatabaseData *db_data = (DatabaseData *) user_data;
    gcry_free (db_data->key);
    g_slist_free_full (db_data->objects_hash, g_free);
    json_node_free (db_data->json_data);
    g_free (db_data);
}
