#include <gtk/gtk.h>
#include <gcrypt.h>
#include <jansson.h>
#include <glib/gstdio.h>
#include "otpclient.h"
#include "common.h"
#include "gquarks.h"
#include "imports.h"
#include "exports.h"
#include "message-dialogs.h"
#include "password-cb.h"
#include "get-builder.h"
#include "liststore-misc.h"

#ifndef USE_FLATPAK_APP_FOLDER
static gchar     *get_db_path               (GtkWidget          *window);
#endif

static void       get_config_data           (gint               *width, 
                                             gint               *height, 
                                             AppData            *app_data);

static void       create_main_window        (gint                width,
                                             gint                height,
                                             AppData            *app_data);

static gboolean   set_action_group          (GtkBuilder         *builder,
                                             AppData            *app_data);

static void       get_window_size_cb        (GtkWidget          *window,
                                             GtkAllocation      *allocation,
                                             gpointer            user_data);

static void       toggle_delete_button_cb   (GtkWidget          *main_window,
                                             gpointer            user_data);

static void       del_data_cb               (GtkToggleButton    *btn,
                                             gpointer            user_data);

static void       change_password_cb        (GSimpleAction      *simple,
                                             GVariant           *parameter,
                                             gpointer            user_data);

static void       save_window_size          (gint                width,
                                             gint                height);

static void       destroy_cb                (GtkWidget          *window,
                                             gpointer            user_data);


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

    AppData *app_data = g_new0 (AppData, 1);

    gint width = 0, height = 0;
    app_data->show_next_otp = FALSE;
    app_data->disable_notifications = FALSE;
    app_data->search_column = 0;
    get_config_data (&width, &height, app_data);

    app_data->db_data = g_new0 (DatabaseData, 1);

    app_data->builder = get_builder_from_partial_path (UI_PARTIAL_PATH);

    create_main_window (width, height, app_data);
    if (app_data->main_window == NULL) {
        g_printerr ("Couldn't locate the ui file, exiting...\n");
        g_free (app_data->db_data);
        g_application_quit (G_APPLICATION(app));
        return;
    }
    gtk_application_add_window (GTK_APPLICATION(app), GTK_WINDOW(app_data->main_window));
    g_signal_connect (app_data->main_window, "size-allocate", G_CALLBACK(get_window_size_cb), NULL);

    if (!gcry_check_version ("1.6.0")) {
        show_message_dialog (app_data->main_window, "The required version of GCrypt is 1.6.0 or greater.", GTK_MESSAGE_ERROR);
        g_free (app_data->db_data);
        g_application_quit (G_APPLICATION(app));
        return;
    }

    if (gcry_control (GCRYCTL_INIT_SECMEM, max_file_size, 0)) {
        show_message_dialog (app_data->main_window, "Couldn't initialize secure memory.\n", GTK_MESSAGE_ERROR);
        g_free (app_data->db_data);
        g_application_quit (G_APPLICATION(app));
        return;
    }
    gcry_control (GCRYCTL_INITIALIZATION_FINISHED, 0);

    json_set_alloc_funcs (gcry_malloc_secure, gcry_free);

#ifdef USE_FLATPAK_APP_FOLDER
    app_data->db_data->db_path = g_build_filename (g_get_user_data_dir (), "otpclient-db.enc", NULL);
    // on the first run the cfg file is not created in the flatpak version because we use a non-changeable db path
    gchar *cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
    if (!g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
        g_file_set_contents (cfg_file_path, "[config]", -1, NULL);
    }
    g_free (cfg_file_path);
#else
    app_data->db_data->db_path = get_db_path (app_data->main_window);
    if (app_data->db_data->db_path == NULL) {
        g_free (app_data->db_data);
        g_application_quit (G_APPLICATION(app));
        return;
    }
#endif

    app_data->db_data->max_file_size_from_memlock = max_file_size;
    app_data->db_data->objects_hash = NULL;
    app_data->db_data->data_to_add = NULL;
    // subtract 3 seconds from the current time. Needed for "last_hotp" to be set on the first run
    app_data->db_data->last_hotp_update = g_date_time_add_seconds (g_date_time_new_now_local (), -(G_TIME_SPAN_SECOND * HOTP_RATE_LIMIT_IN_SEC));

    retry:
    app_data->db_data->key = prompt_for_password (app_data, NULL, NULL, FALSE);
    if (app_data->db_data->key == NULL) {
        g_free (app_data->db_data);
        g_application_quit (G_APPLICATION(app));
        return;
    }

    GError *err = NULL;
    load_db (app_data->db_data, &err);
    if (err != NULL && !g_error_matches (err, missing_file_gquark (), MISSING_FILE_CODE)) {
        show_message_dialog (app_data->main_window, err->message, GTK_MESSAGE_ERROR);
        gcry_free (app_data->db_data->key);
        goto retry;
    }

    if (g_error_matches (err, missing_file_gquark(), MISSING_FILE_CODE) || json_array_size (app_data->db_data->json_data) == 0) {
        const gchar *msg = "This is the first time you run OTPClient, so you need to <b>add</b> or <b>import</b> some tokens.\n"
        "- to <b>add</b> tokens, please click the + button on the <b>top left</b>.\n"
        "- to <b>import</b> existing tokens, please click the menu button <b>on the top right</b>.\n"
        "\nIf you need more info, please visit the <a href=\"https://github.com/paolostivanin/OTPClient/wiki\">project's wiki</a>";
        show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_INFO);
    }

    app_data->clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);
    
    create_treeview (app_data);
    
    app_data->notification = g_notification_new ("OTPClient");
    g_notification_set_priority (app_data->notification, G_NOTIFICATION_PRIORITY_NORMAL);
    GIcon *icon = g_themed_icon_new ("com.github.paolostivanin.OTPClient");
    g_notification_set_icon (app_data->notification, icon);
    g_notification_set_body (app_data->notification, "OTP value has been copied to the clipboard");
    g_object_unref (icon);

    GtkToggleButton *del_toggle_btn = GTK_TOGGLE_BUTTON(gtk_builder_get_object (app_data->builder, "del_toggle_btn_id"));

    g_signal_new ("toggle-delete-button", G_TYPE_OBJECT, G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    GtkBindingSet *toggle_btn_binding_set = gtk_binding_set_by_class (GTK_APPLICATION_WINDOW_GET_CLASS (app_data->main_window));
    gtk_binding_entry_add_signal (toggle_btn_binding_set, GDK_KEY_d, GDK_CONTROL_MASK, "toggle-delete-button", 0);
    g_signal_connect (app_data->main_window, "toggle-delete-button", G_CALLBACK(toggle_delete_button_cb), del_toggle_btn);
    g_signal_connect (del_toggle_btn, "toggled", G_CALLBACK(del_data_cb), app_data);

    g_signal_connect (app_data->main_window, "destroy", G_CALLBACK(destroy_cb), app_data);

    app_data->source_id = g_timeout_add_full (G_PRIORITY_DEFAULT, 500, traverse_liststore, app_data, NULL);

    gtk_widget_show_all (app_data->main_window);
}


static void
get_config_data (gint     *width,
                 gint     *height,
                 AppData  *app_data)
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
            *width = g_key_file_get_integer (kf, "config", "window_width", NULL);
            *height = g_key_file_get_integer (kf, "config", "window_height", NULL);
            app_data->show_next_otp = g_key_file_get_boolean (kf, "config", "show_next_otp", NULL);
            app_data->disable_notifications = g_key_file_get_boolean (kf, "config", "notifications", NULL);
            app_data->search_column = g_key_file_get_integer (kf, "config", "search_column", NULL);
        }
    }
    g_key_file_free (kf);
    g_free (cfg_file_path);
}


static void
create_main_window (gint             width,
                    gint             height,
                    AppData         *app_data)
{
    app_data->main_window = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "appwindow_id"));
    gtk_window_set_icon_name (GTK_WINDOW(app_data->main_window), "otpclient");

    gtk_window_set_default_size (GTK_WINDOW(app_data->main_window), (width >= 150) ? width : 500, (height >= 150) ? height : 300);

    GtkWidget *header_bar =  GTK_WIDGET(gtk_builder_get_object (app_data->builder, "headerbar_id"));
    gtk_header_bar_set_subtitle (GTK_HEADER_BAR(header_bar), APP_VERSION);

    set_action_group (app_data->builder, app_data);
}


static gboolean
set_action_group (GtkBuilder *builder,
                  AppData    *app_data)
{
    static GActionEntry settings_menu_entries[] = {
            { .name = ANDOTP_IMPORT_ACTION_NAME, .activate = select_file_cb },
            { .name = AUTHPLUS_IMPORT_ACTION_NAME, .activate = select_file_cb },
            { .name = ANDOTP_EXPORT_ACTION_NAME, .activate = export_data_cb },
            { .name = "change_pwd", .activate = change_password_cb },
            { .name = "edit_row", .activate = edit_selected_row_cb },
            { .name = "settings", .activate = settings_dialog_cb },
            { .name = "shortcuts", .activate = shortcuts_window_cb }
    };

    static GActionEntry add_menu_entries[] = {
            { .name = "webcam", .activate = webcam_cb },
            { .name = "screenshot", .activate = screenshot_cb },
            { .name = "select_photo", .activate = select_photo_cb },
            { .name = "manual", .activate = add_data_dialog }
    };

    GtkWidget *settings_popover = GTK_WIDGET (gtk_builder_get_object (builder, "settings_pop_id"));
    GActionGroup *settings_actions = (GActionGroup *)g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (settings_actions), settings_menu_entries, G_N_ELEMENTS (settings_menu_entries), app_data);
    gtk_widget_insert_action_group (settings_popover, "settings_menu", settings_actions);

    GtkWidget *add_popover = GTK_WIDGET (gtk_builder_get_object (builder, "add_pop_id"));
    GActionGroup *add_actions = (GActionGroup *)g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (add_actions), add_menu_entries, G_N_ELEMENTS (add_menu_entries), app_data);
    gtk_widget_insert_action_group (add_popover, "add_menu", add_actions);

#if GTK_CHECK_VERSION(3, 20, 0)
    gtk_popover_set_constrain_to (GTK_POPOVER(add_popover), GTK_POPOVER_CONSTRAINT_NONE);
    gtk_popover_set_constrain_to (GTK_POPOVER(settings_popover), GTK_POPOVER_CONSTRAINT_NONE);
#endif

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
toggle_delete_button_cb (GtkWidget *main_window __attribute__((unused)),
                         gpointer   user_data)
{
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(user_data), !gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON(user_data)));
}


static void
del_data_cb (GtkToggleButton *btn,
             gpointer         user_data)
{
    AppData *app_data = (AppData *)user_data;

    GtkStyleContext *gsc = gtk_widget_get_style_context (GTK_WIDGET(btn));
    GtkTreeSelection *tree_selection = gtk_tree_view_get_selection (app_data->tree_view);

    if (gtk_toggle_button_get_active (btn)) {
        app_data->css_provider = gtk_css_provider_new ();
        gtk_css_provider_load_from_data (app_data->css_provider, "#delbtn { background: #ff0033; }", -1, NULL);
        gtk_style_context_add_provider (gsc, GTK_STYLE_PROVIDER(app_data->css_provider), GTK_STYLE_PROVIDER_PRIORITY_USER);
        AppData *app_data = (AppData *)user_data;
        const gchar *msg = "You just entered the deletion mode. You can now click on the row(s) you'd like to delete.\n"
            "Please note that once a row has been deleted, <b>it's impossible to recover the associated data.</b>";

        if (get_confirmation_from_dialog (app_data->main_window, msg)) {
            g_signal_handlers_disconnect_by_func (app_data->tree_view, row_selected_cb, app_data);
            // the following function emits the "changed" signal
            gtk_tree_selection_unselect_all (tree_selection);
            // clear all active otps before proceeding to the deletion phase
            g_signal_emit_by_name (app_data->tree_view, "hide-all-otps");
            g_signal_connect (app_data->tree_view, "row-activated", G_CALLBACK(delete_rows_cb), app_data);
        } else {
            gtk_toggle_button_set_active (btn, FALSE);
        }
    } else {
        gtk_style_context_remove_provider (gsc, GTK_STYLE_PROVIDER(app_data->css_provider));
        g_object_unref (app_data->css_provider);
        g_signal_handlers_disconnect_by_func (app_data->tree_view, delete_rows_cb, app_data);
        g_signal_connect (app_data->tree_view, "row-activated", G_CALLBACK(row_selected_cb), app_data);
    }
}


static void
change_password_cb (GSimpleAction *simple    __attribute__((unused)),
                    GVariant      *parameter __attribute__((unused)),
                    gpointer       user_data)
{
    AppData *app_data = (AppData *)user_data;
    gchar *tmp_key = secure_strdup (app_data->db_data->key);
    gchar *pwd = prompt_for_password (app_data, tmp_key, NULL, FALSE);
    if (pwd != NULL) {
        app_data->db_data->key = pwd;
        GError *err = NULL;
        update_and_reload_db (app_data, FALSE, &err);
        if (err != NULL) {
            show_message_dialog (app_data->main_window, err->message, GTK_MESSAGE_ERROR);
            GtkApplication *app = gtk_window_get_application (GTK_WINDOW (app_data->main_window));
            destroy_cb (app_data->main_window, app_data);
            g_application_quit (G_APPLICATION(app));
        }
        show_message_dialog (app_data->main_window, "Password successfully changed", GTK_MESSAGE_INFO);
    } else {
        gcry_free (tmp_key);
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
    AppData *app_data = (AppData *)user_data;
    g_source_remove (app_data->source_id);
    gcry_free (app_data->db_data->key);
    g_free (app_data->db_data->db_path);
    g_slist_free_full (app_data->db_data->objects_hash, g_free);
    json_decref (app_data->db_data->json_data);
    g_free (app_data->db_data);
    gtk_clipboard_clear (app_data->clipboard);
    g_application_withdraw_notification (G_APPLICATION(gtk_window_get_application (GTK_WINDOW(app_data->main_window))), NOTIFICATION_ID);
    g_object_unref (app_data->notification);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wbad-function-cast"
    gint w = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (window), "width"));
    gint h = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (window), "height"));
#pragma GCC diagnostic pop
    save_window_size (w, h);
    g_object_unref (app_data->builder);
    g_free (app_data);
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
