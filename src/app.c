#include <gtk/gtk.h>
#include <gcrypt.h>
#include <jansson.h>
#include "otpclient.h"
#include "common.h"
#include "gquarks.h"
#include "imports.h"
#include "message-dialogs.h"
#include "password-cb.h"

#define gcry_calloc_secure_short(n) gcry_calloc_secure(n, 1)

#ifndef USE_FLATPAK_APP_FOLDER
static gchar     *get_db_path                        (GtkWidget *window);
#endif

static void       get_saved_window_size              (gint *width, gint *height);

static GtkWidget *create_main_window_with_header_bar (GtkApplication *app, gint width, gint height, ImportData *import_data);

static gboolean   add_popover_to_button              (GtkWidget *button, ImportData *import_data);

static void       get_window_size_cb                 (GtkWidget *window, GtkAllocation *allocation, gpointer user_data);

static void       add_data_cb                        (GtkWidget *btn, gpointer user_data);

static void       del_data_cb                        (GtkWidget *btn, gpointer user_data);

static void       change_password_cb                 (GSimpleAction *simple, GVariant *parameter, gpointer user_data);

static void       save_window_size                   (gint width, gint height);

static void       destroy_cb                         (GtkWidget *window, gpointer user_data);


void
activate (GtkApplication    *app,
          gpointer           user_data)
{
    gint64 memlock_limit = (gint64) user_data;
    gint32 max_file_size;
    if (memlock_limit == -1 || memlock_limit > 256000) {
        max_file_size = 256000; // memlock is either unlimited or bigger than needed
    } else if (memlock_limit == -5) {
        max_file_size = 64000; // couldn't get memlock limit, so falling back to a default, low value
        g_print ("[WARNING] your OS's memlock limit may be too low for you. Please have a look at https://github.com/paolostivanin/OTPClient#limitations\n.");
    } else {
        max_file_size = (gint32) memlock_limit; // memlock is less than 256 KB
        g_print ("[WARNING] your OS's memlock limit may be too low for you. Please have a look at https://github.com/paolostivanin/OTPClient#limitations\n.");
    }

    gint width, height;
    get_saved_window_size (&width, &height);

    ImportData *import_data = g_new0 (ImportData, 1);
    DatabaseData *db_data = g_new0 (DatabaseData, 1);
    import_data->db_data = db_data;

    GtkWidget *main_window = create_main_window_with_header_bar (app, width, height, import_data);
    if (main_window == NULL) {
        g_printerr ("Couldn't locate the ui file, exiting...\n");
        g_free (db_data);
        g_application_quit (G_APPLICATION (app));
        return;
    }
    gtk_application_add_window (GTK_APPLICATION (app), GTK_WINDOW (main_window));
    g_signal_connect (main_window, "size-allocate", G_CALLBACK (get_window_size_cb), NULL);

    if (!gcry_check_version ("1.6.0")) {
        show_message_dialog (main_window, "The required version of GCrypt is 1.6.0 or greater.", GTK_MESSAGE_ERROR);
        g_free (db_data);
        g_application_quit (G_APPLICATION (app));
        return;
    }

    if (gcry_control (GCRYCTL_INIT_SECMEM, max_file_size, 0)) {
        show_message_dialog (main_window, "Couldn't initialize secure memory.\n", GTK_MESSAGE_ERROR);
        g_free (db_data);
        g_application_quit (G_APPLICATION (app));
        return;
    }
    gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);

    json_set_alloc_funcs (gcry_calloc_secure_short, gcry_free);

#ifdef USE_FLATPAK_APP_FOLDER
    db_data->db_path = g_build_filename (g_get_user_data_dir (), "otpclient-db.enc", NULL);
#else
    db_data->db_path = get_db_path (main_window);
    if (db_data->db_path == NULL) {
        g_free (db_data);
        g_application_quit (G_APPLICATION (app));
        return;
    }
#endif

    db_data->max_file_size_from_memlock = max_file_size;
    db_data->objects_hash = NULL;
    db_data->data_to_add = NULL;
    // subtract 3 seconds from the current time. Needed for "last_hotp" to be set on the first run
    db_data->last_hotp_update = g_date_time_add_seconds (g_date_time_new_now_local (), -(G_TIME_SPAN_SECOND * HOTP_RATE_LIMIT_IN_SEC));

    db_data->key = prompt_for_password (main_window, g_file_test (db_data->db_path, G_FILE_TEST_EXISTS));
    if (db_data->key == NULL) {
        g_free (db_data);
        g_application_quit (G_APPLICATION (app));
        return;
    }

    GError *err = NULL;
    load_db (db_data, &err);
    if (err != NULL && !g_error_matches (err, missing_file_gquark (), MISSING_FILE_CODE)) {
        show_message_dialog (main_window, err->message, GTK_MESSAGE_ERROR);
        gcry_free (db_data->key);
        g_free (db_data);
        g_application_quit (G_APPLICATION (app));
        return;
    }

    GtkClipboard *clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
    GtkListStore *list_store = create_treeview (main_window, clipboard, db_data);
    g_object_set_data (G_OBJECT (main_window), "lstore", list_store);
    g_object_set_data (G_OBJECT (main_window), "clipboard", clipboard);

    g_signal_connect (find_widget (main_window, "add_btn_app"), "clicked", G_CALLBACK (add_data_cb), db_data);
    g_signal_connect (find_widget (main_window, "del_btn_app"), "clicked", G_CALLBACK (del_data_cb), db_data);
    g_signal_connect (main_window, "destroy", G_CALLBACK (destroy_cb), import_data);

    gtk_widget_show_all (main_window);
}


static void
get_saved_window_size (gint *width, gint *height)
{
    GError *err = NULL;
    GKeyFile *kf = g_key_file_new ();
    gchar *cfg_file_path;
#ifndef USE_FLATPAK_APP_FOLDER
    cfg_file_path = g_build_filename (g_get_home_dir (), ".config", "otpclient.cfg", NULL);
#else
    cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#endif
    *width = 0;
    *height = 0;
    if (g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
        if (!g_key_file_load_from_file (kf, cfg_file_path, G_KEY_FILE_NONE, &err)) {
            g_printerr ("%s\n", err->message);
        } else {
            *width = g_key_file_get_integer (kf, "config", "window_width", NULL);
            *height = g_key_file_get_integer (kf, "config", "window_height", NULL);
        }
    }
    g_key_file_free (kf);
    g_free (cfg_file_path);
}


static GtkWidget *
create_main_window_with_header_bar (GtkApplication  *app,
                                    gint             width,
                                    gint             height,
                                    ImportData      *import_data)
{
    GtkWidget *window = gtk_application_window_new (app);
    gtk_window_set_position (GTK_WINDOW (window), GTK_WIN_POS_CENTER);
    gtk_window_set_icon_name (GTK_WINDOW (window), "otpclient");

    import_data->main_window = window;
    
    gtk_container_set_border_width (GTK_CONTAINER (window), 10);

    if (width > 0 && height > 0) {
        gtk_window_set_default_size (GTK_WINDOW (window), width, height);
    } else {
        gtk_window_set_default_size (GTK_WINDOW (window), 500, 350);
    }

    gchar *header_bar_text = g_malloc (strlen (APP_NAME ) + 1 + strlen (APP_VERSION) + 1);
    g_snprintf (header_bar_text, strlen (APP_NAME) + 1 + strlen (APP_VERSION) + 1, "%s %s", APP_NAME, APP_VERSION);
    header_bar_text[strlen(header_bar_text)] = '\0';

    GtkWidget *header_bar = create_header_bar (header_bar_text);
    GtkWidget *box = create_box_with_buttons ("add_btn_app", "del_btn_app");
    gtk_header_bar_pack_start (GTK_HEADER_BAR (header_bar), box);
    GtkWidget *settings_btn = gtk_menu_button_new ();
    gtk_widget_set_name (settings_btn, "settings_btn_app");
    gtk_container_add (GTK_CONTAINER (settings_btn), gtk_image_new_from_icon_name ("open-menu-symbolic", GTK_ICON_SIZE_BUTTON));
    if (!add_popover_to_button (settings_btn, import_data)) {
        return NULL;
    }
    gtk_header_bar_pack_end (GTK_HEADER_BAR (header_bar), settings_btn);

    gtk_window_set_titlebar (GTK_WINDOW (window), header_bar);

    g_free (header_bar_text);

    return window;
}


static gboolean
add_popover_to_button (GtkWidget    *button,
                       ImportData   *import_data)
{
    static GActionEntry menu_entries[] = {
            { .name = ANDOTP_IMPORT_ACTION_NAME, .activate = select_file_cb },
            { .name = AUTHPLUS_IMPORT_ACTION_NAME, .activate = select_file_cb },
            { .name = "export", .activate = NULL },
            { .name = "change_pwd", .activate = change_password_cb }
    };

    const gchar *prefix;
    const gchar *partial_path = "share/otpclient/popover.ui";
#ifndef USE_FLATPAK_APP_FOLDER
    if (g_file_test (g_strconcat ("/usr/", partial_path, NULL), G_FILE_TEST_EXISTS)) {
        prefix = "/usr/";
    } else if (g_file_test (g_strconcat ("/usr/local/", partial_path, NULL), G_FILE_TEST_EXISTS)) {
        prefix = "/usr/local/";
    } else {
        return FALSE;
    }
#else
    prefix = "/app/";
#endif
    gchar *path = g_strconcat (prefix, partial_path, NULL);
    GtkBuilder *builder = gtk_builder_new_from_file (path);
    g_free (path);
    GtkWidget *popover = GTK_WIDGET (gtk_builder_get_object (builder, "pop1"));

    GActionGroup *actions = (GActionGroup *)g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (actions), menu_entries, G_N_ELEMENTS (menu_entries), import_data);
    gtk_widget_insert_action_group (popover, "menu", actions);

    gtk_menu_button_set_popover (GTK_MENU_BUTTON (button), popover);
    g_object_unref (builder);

    return TRUE;
}


#ifndef USE_FLATPAK_APP_FOLDER
static gchar *
get_db_path (GtkWidget *window)
{
    gchar *db_path = NULL;
    GError *err = NULL;
    GKeyFile *kf = g_key_file_new ();
    gchar *cfg_file_path = g_build_filename (g_get_home_dir (), ".config", "otpclient.cfg", NULL);
    if (g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
        if (!g_key_file_load_from_file (kf, cfg_file_path, G_KEY_FILE_NONE, &err)) {
            g_printerr ("%s\n", err->message);
            g_key_file_free (kf);
            return NULL;
        }
        db_path = g_key_file_get_string (kf, "config", "db_path", &err);
        if (db_path == NULL) {
            g_printerr ("%s\n", err->message);
            g_key_file_free (kf);
            return NULL;
        }
    } else {
        GtkWidget *dialog = gtk_file_chooser_dialog_new ("Select database location",
                                                         GTK_WINDOW (window),
                                                         GTK_FILE_CHOOSER_ACTION_SAVE,
                                                         "Cancel", GTK_RESPONSE_CANCEL,
                                                         "OK", GTK_RESPONSE_ACCEPT,
                                                         NULL);
        GtkFileChooser *chooser = GTK_FILE_CHOOSER (dialog);
        gtk_file_chooser_set_do_overwrite_confirmation (chooser, TRUE);
        gtk_file_chooser_set_select_multiple (chooser, FALSE);
        gtk_file_chooser_set_current_name (chooser, "NewDatabase.enc");
        gint res = gtk_dialog_run (GTK_DIALOG (dialog));
        if (res == GTK_RESPONSE_ACCEPT) {
            db_path = gtk_file_chooser_get_filename (chooser);
            g_key_file_set_string (kf, "config", "db_path", db_path);
            g_key_file_save_to_file (kf, cfg_file_path, &err);
            if (err != NULL) {
                g_printerr ("%s\n", err->message);
                g_key_file_free (kf);
            }
        }
        gtk_widget_destroy (dialog);
    }

    g_free (cfg_file_path);

    return db_path;
}
#endif


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
change_password_cb (GSimpleAction *simple    __attribute__((unused)),
                    GVariant      *parameter __attribute__((unused)),
                    gpointer       user_data)
{
    ImportData *import_data = (ImportData *)user_data;
    gcry_free (import_data->db_data->key);
    import_data->db_data->key = prompt_for_password (import_data->main_window, FALSE);
    if (import_data->db_data->key != NULL) {
        GError *err = NULL;
        update_and_reload_db (import_data->db_data, NULL, FALSE, &err);
        if (err != NULL) {
            show_message_dialog (import_data->main_window, err->message, GTK_MESSAGE_ERROR);
            GtkApplication *app = gtk_window_get_application (GTK_WINDOW (import_data->main_window));
            destroy_cb (import_data->main_window, import_data);
            g_application_quit (G_APPLICATION (app));
        }
    }
}


static void
get_window_size_cb (GtkWidget      *window,
                    GtkAllocation  *allocation __attribute__((unused)),
                    gpointer        user_data  __attribute__((unused)))
{
    gint w, h;
    gtk_window_get_size (GTK_WINDOW (window), &w, &h);
    g_object_set_data (G_OBJECT (window), "width", GINT_TO_POINTER (w));
    g_object_set_data (G_OBJECT (window), "height", GINT_TO_POINTER (h));
}


static void
destroy_cb (GtkWidget   *window,
            gpointer     user_data)
{
    ImportData *import_data = (ImportData *)user_data;
    gcry_free (import_data->db_data->key);
    g_free (import_data->db_data->db_path);
    g_slist_free_full (import_data->db_data->objects_hash, g_free);
    json_decref (import_data->db_data->json_data);
    g_free (import_data->db_data);
    g_free (import_data);
    gtk_clipboard_clear (g_object_get_data (G_OBJECT (window), "clipboard"));
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wbad-function-cast"
    gint w = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (window), "width"));
    gint h = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (window), "height"));
#pragma GCC diagnostic pop
    save_window_size (w, h);
}


static void
save_window_size (gint width,
                  gint height)
{
    GError *err = NULL;
    GKeyFile *kf = g_key_file_new ();
    gchar *cfg_file_path;
#ifndef USE_FLATPAK_APP_FOLDER
    cfg_file_path = g_build_filename (g_get_home_dir (), ".config", "otpclient.cfg", NULL);
#else
    cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#endif
    if (g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
        if (!g_key_file_load_from_file (kf, cfg_file_path, G_KEY_FILE_NONE, &err)) {
            g_printerr ("%s\n", err->message);
        } else {
            g_key_file_set_integer (kf, "config", "window_width", width);
            g_key_file_set_integer (kf, "config", "window_height", height);
            if (!g_key_file_save_to_file (kf, cfg_file_path, &err)) {
                g_printerr ("%s\n", err->message);
            }
        }
    }
    g_key_file_free (kf);
    g_free (cfg_file_path);
}
