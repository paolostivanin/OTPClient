#include <gtk/gtk.h>
#include <gcrypt.h>
#include "otpclient.h"
#include "common.h"
#include "gquarks.h"
#include "imports.h"
#include "message-dialogs.h"
#include "password-cb.h"

static GtkWidget *create_main_window_with_header_bar (GtkApplication *app);

static void add_data_cb (GtkWidget *btn, gpointer user_data);

static void del_data_cb (GtkWidget *btn, gpointer user_data);

static void import_cb (GtkWidget *btn, gpointer user_data);

static void destroy_cb (GtkWidget *window, gpointer user_data);


void
activate (GtkApplication    *app,
          gpointer           user_data)
{
    gint64 memlock_limit = (gint64) user_data;
    gint32 max_file_size;
    if (memlock_limit == -1 || memlock_limit > 256000) {
        max_file_size = 256000; // memlock is either unlimited or bigger than what we need
    } else if (memlock_limit == -5) {
        max_file_size = 64000; // couldn't get memlock limit, so falling back to a default, low value
        g_print ("[WARNING] your OS's memlock limit may be too low for you. Please have a look at https://github.com/paolostivanin/OTPClient#limitations\n.");
    } else {
        max_file_size = (gint32) memlock_limit; // memlock is less than 256 KB
        g_print ("[WARNING] your OS's memlock limit may be too low for you. Please have a look at https://github.com/paolostivanin/OTPClient#limitations\n.");
    }

    GtkWidget *main_window = create_main_window_with_header_bar (app);
    gtk_application_add_window (GTK_APPLICATION (app), GTK_WINDOW (main_window));

    if (!gcry_check_version ("1.6.0")) {
        show_message_dialog (main_window, "The required version of GCrypt is 1.6.0 or greater.", GTK_MESSAGE_ERROR);
        gtk_application_remove_window (GTK_APPLICATION (app), GTK_WINDOW (main_window));
        return;
    }

    if (gcry_control (GCRYCTL_INIT_SECMEM, max_file_size, 0)) {
        show_message_dialog (main_window, "Couldn't initialize secure memory.\n", GTK_MESSAGE_ERROR);
        gtk_application_remove_window (GTK_APPLICATION (app), GTK_WINDOW (main_window));
        return;
    }
    gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);

    DatabaseData *db_data = g_new0 (DatabaseData, 1);
    db_data->max_file_size_from_memlock = max_file_size;
    db_data->objects_hash = NULL;
    db_data->data_to_add = NULL;
    // subtract 3 seconds from the current time. Needed for "last_hotp" to be set on the first run
    db_data->last_hotp_update = g_date_time_add_seconds (g_date_time_new_now_local (), -(G_TIME_SPAN_SECOND * HOTP_RATE_LIMIT_IN_SEC));
    gchar *db_path = g_strconcat (g_get_user_config_dir (), "/", DB_FILE_NAME, NULL);
    db_data->key = prompt_for_password (main_window, g_file_test (db_path, G_FILE_TEST_EXISTS));
    g_free (db_path);
    if (db_data->key == NULL) {
        gtk_application_remove_window (GTK_APPLICATION (app), GTK_WINDOW (main_window));
        return;
    }

    GError *err = NULL;
    load_db (db_data, &err);
    if (err != NULL && !g_error_matches (err, missing_file_gquark (), MISSING_FILE_CODE)) {
        show_message_dialog (main_window, err->message, GTK_MESSAGE_ERROR);
        gcry_free (db_data->key);
        g_free (db_data);
        gtk_application_remove_window (GTK_APPLICATION (app), GTK_WINDOW (main_window));
        return;
    }

    GtkListStore *list_store = create_treeview (main_window, db_data);
    g_object_set_data (G_OBJECT (main_window), "lstore", list_store);

    g_signal_connect (find_widget (main_window, "add_btn_app"), "clicked", G_CALLBACK (add_data_cb), db_data);
    g_signal_connect (find_widget (main_window, "del_btn_app"), "clicked", G_CALLBACK (del_data_cb), db_data);
    g_signal_connect (find_widget (main_window, "imp_btn_app"), "clicked", G_CALLBACK (import_cb), db_data);
    g_signal_connect (main_window, "destroy", G_CALLBACK (destroy_cb), db_data);

    gtk_widget_show_all (main_window);
}


static GtkWidget *
create_main_window_with_header_bar (GtkApplication  *app)
{
    GtkWidget *window = gtk_application_window_new (app);
    gtk_window_set_position (GTK_WINDOW (window), GTK_WIN_POS_CENTER);
    gtk_window_set_resizable (GTK_WINDOW (window), FALSE);

    gtk_window_set_icon_name (GTK_WINDOW (window), "otpclient");
    
    gtk_container_set_border_width (GTK_CONTAINER (window), 10);

    gtk_widget_set_size_request (GTK_WIDGET (window), 475, 360);
    gtk_window_set_resizable (GTK_WINDOW (window), TRUE);

    gchar *header_bar_text = g_malloc (strlen (APP_NAME ) + 1 + strlen (APP_VERSION) + 1);
    g_snprintf (header_bar_text, strlen (APP_NAME) + 1 + strlen (APP_VERSION) + 1, "%s %s", APP_NAME, APP_VERSION);
    header_bar_text[strlen(header_bar_text)] = '\0';

    GtkWidget *header_bar = create_header_bar (header_bar_text);
    GtkWidget *box = create_box_with_buttons ("add_btn_app", "del_btn_app", "imp_btn_app");
    gtk_header_bar_pack_start (GTK_HEADER_BAR (header_bar), box);
    gtk_window_set_titlebar (GTK_WINDOW (window), header_bar);

    g_free (header_bar_text);

    return window;
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
del_data_cb (GtkWidget *btn,
             gpointer   user_data)
{
    GtkWidget *top_level = gtk_widget_get_toplevel (btn);
    DatabaseData *db_data = (DatabaseData *) user_data;
    GtkListStore *list_store = g_object_get_data (G_OBJECT (top_level), "lstore");
    const gchar *msg = "Do you really want to delete the selected entries?\n"
            "This will <b>permanently</b> delete these entries from the database";

    if (get_confirmation_from_dialog (top_level, msg)) {
        remove_selected_entries (db_data, list_store);
    }
}


static void
import_cb   (GtkWidget *btn,
             gpointer   user_data)
{
    DatabaseData *db_data = (DatabaseData *) user_data;

    GtkWidget *popover = gtk_popover_new (btn);
    gtk_popover_set_position (GTK_POPOVER (popover), GTK_POS_TOP);

    GtkWidget *button_box = gtk_button_box_new (GTK_ORIENTATION_VERTICAL);
    gtk_container_add (GTK_CONTAINER (popover), button_box);

    GtkWidget *bt1 = gtk_button_new_with_label ("andOTP");
    gtk_widget_set_name (bt1, ANDOTP_BTN_NAME);
    gtk_container_add(GTK_CONTAINER (button_box), bt1);
    g_signal_connect (bt1, "clicked", G_CALLBACK (select_file_cb), db_data);

    GtkWidget *bt2 = gtk_button_new_with_label ("Authenticator Plus");
    gtk_widget_set_name (bt2, AUTHPLUS_BTN_NAME);
    gtk_container_add(GTK_CONTAINER (button_box), bt2);
    g_signal_connect (bt2, "clicked", G_CALLBACK (select_file_cb), db_data);

    gtk_widget_show_all (popover);
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