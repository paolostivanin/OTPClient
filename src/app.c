#include <gtk/gtk.h>
#include <gcrypt.h>
#include <jansson.h>
#include <libsecret/secret.h>
#include <glib/gi18n.h>
#include "otpclient.h"
#include "gquarks.h"
#include "imports.h"
#include "common/exports.h"
#include "message-dialogs.h"
#include "password-cb.h"
#include "get-builder.h"
#include "liststore-misc.h"
#include "lock-app.h"
#include "change-db-cb.h"
#include "new-db-cb.h"
#include "common/common.h"
#include "secret-schema.h"
#include "change-pwd-cb.h"
#include "settings-cb.h"
#include "shortcuts-cb.h"
#include "webcam-add-cb.h"
#include "manual-add-cb.h"
#include "edit-row-cb.h"
#include "show-qr-cb.h"
#include "dbinfo-cb.h"

#ifndef USE_FLATPAK_APP_FOLDER
static gchar     *get_db_path               (AppData            *app_data);
#endif

static void       set_config_data           (gint               *width,
                                             gint               *height,
                                             AppData            *app_data);

static void      migrate_secretservice_kf   (AppData            *app_data,
                                             GKeyFile           *kf,
                                             gboolean            value);

static gboolean   get_warn_data             (void);

static void       set_warn_data             (gboolean            show_warning);

static void       create_main_window        (gint                width,
                                             gint                height,
                                             AppData            *app_data);

static gboolean   show_upgrade_msg          (void);

static void       set_info_bar              (AppData            *app_data,
                                             const gchar        *msg);

static void       on_bar_response           (GtkInfoBar         *ib,
                                             gint                response_id,
                                             gpointer            user_data);

static gboolean   set_action_group          (GtkBuilder         *builder,
                                             AppData            *app_data);

static void       get_window_size_cb        (GtkWidget          *window,
                                             GtkAllocation      *allocation,
                                             gpointer            user_data);

void              setup_kb_shortcuts        (AppData            *app_data);

static void       toggle_button_cb          (GtkWidget          *main_window,
                                             gpointer            user_data);

static void       reorder_rows_cb           (GtkToggleButton *btn,
                                             gpointer         user_data);

static void       del_data_cb               (GtkToggleButton    *btn,
                                             gpointer            user_data);

static void       save_sort_order           (GtkTreeView        *tree_view);

static void       save_window_size          (gint                width,
                                             gint                height);

static void       store_data                (const gchar        *param1_name,
                                             gint                param1_value,
                                             const gchar        *param2_name,
                                             gint                param2_value);

static gboolean   key_pressed_cb            (GtkWidget          *window,
                                             GdkEventKey        *event_key,
                                             gpointer            user_data);

static gboolean   show_memlock_warn_dialog  (gint32              max_file_size,
                                             GtkBuilder         *builder);

static void       set_open_db_action        (GtkWidget          *btn,
                                             gpointer            user_data);


void
activate (GtkApplication    *app,
          gpointer           user_data __attribute__((unused)))
{
    gint32 max_file_size = get_max_file_size_from_memlock ();

    AppData *app_data = g_new0 (AppData, 1);

    app_data->app_locked = FALSE;

    gint width = 0, height = 0;
    app_data->show_next_otp = FALSE; // next otp not shown by default
    app_data->disable_notifications = FALSE; // notifications enabled by default
    app_data->search_column = 0; // account
    app_data->auto_lock = FALSE; // disabled by default
    app_data->inactivity_timeout = 0; // never
    app_data->use_dark_theme = FALSE; // light theme by default
    app_data->use_secret_service = TRUE; // secret service enabled by default
    app_data->is_reorder_active = FALSE; // when app is started, reorder is not set
    // open_db_file_action is set only on first startup and not when the db is deleted but the cfg file is there, therefore we need a default action
    app_data->open_db_file_action = GTK_FILE_CHOOSER_ACTION_SAVE;
    app_data->builder = get_builder_from_partial_path (UI_PARTIAL_PATH);

    set_config_data (&width, &height, app_data);

    app_data->db_data = g_new0 (DatabaseData, 1);
    app_data->db_data->key_stored = FALSE; // at startup, we don't know whether the key is stored or not

    create_main_window (width, height, app_data);
    if (app_data->main_window == NULL) {
        g_printerr ("%s\n", _("Couldn't locate the ui file, exiting..."));
        g_free (app_data->db_data);
        g_application_quit (G_APPLICATION(app));
        return;
    }
    gtk_application_add_window (GTK_APPLICATION(app), GTK_WINDOW(app_data->main_window));
    g_signal_connect (app_data->main_window, "size-allocate", G_CALLBACK(get_window_size_cb), NULL);

    gchar *init_msg = init_libs (max_file_size);
    if (init_msg != NULL) {
        show_message_dialog (app_data->main_window, init_msg, GTK_MESSAGE_ERROR);
        g_free (init_msg);
        g_free (app_data->db_data);
        g_application_quit (G_APPLICATION(app));
        return;
    }

#ifdef USE_FLATPAK_APP_FOLDER
    app_data->db_data->db_path = g_build_filename (g_get_user_data_dir (), "otpclient-db.enc", NULL);
    // on the first run the cfg file is not created in the flatpak version because we use a non-changeable db path
    gchar *cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
    if (!g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
        g_file_set_contents (cfg_file_path, "[config]", -1, NULL);
    }
    g_free (cfg_file_path);
#else
    if (!g_file_test (g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL), G_FILE_TEST_EXISTS)) {
        app_data->diag_rcdb = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "dialog_rcdb_id"));
        GtkWidget *restore_btn = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "diag_rc_restoredb_btn_id"));
        GtkWidget *create_btn = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "diag_rc_createdb_btn_id"));
        g_signal_connect (restore_btn, "clicked", G_CALLBACK (set_open_db_action), app_data);
        g_signal_connect (create_btn, "clicked", G_CALLBACK (set_open_db_action), app_data);

        gint response = gtk_dialog_run (GTK_DIALOG(app_data->diag_rcdb));
        switch (response) {
            case GTK_RESPONSE_CANCEL:
            default:
                gtk_widget_destroy (app_data->diag_rcdb);
                g_free (app_data->db_data);
                g_free (app_data);
                g_application_quit (G_APPLICATION(app));
                return;
            case GTK_RESPONSE_OK:
                gtk_widget_destroy (app_data->diag_rcdb);
        }
    }

    app_data->db_data->db_path = get_db_path (app_data);
    if (app_data->db_data->db_path == NULL) {
        g_free (app_data->db_data);
        g_free (app_data);
        g_application_quit (G_APPLICATION(app));
        return;
    }
#endif

    if (max_file_size < LOW_MEMLOCK_VALUE && get_warn_data () == TRUE) {
        if (show_memlock_warn_dialog (max_file_size, app_data->builder) == TRUE) {
            g_free (app_data->db_data);
            g_free (app_data);
            g_application_quit (G_APPLICATION(app));
            return;
        }
    }

    app_data->db_data->max_file_size_from_memlock = max_file_size;
    app_data->db_data->objects_hash = NULL;
    app_data->db_data->data_to_add = NULL;
    // subtract 3 seconds from the current time. Needed for "last_hotp" to be set on the first run
    app_data->db_data->last_hotp_update = g_date_time_add_seconds (g_date_time_new_now_local (), -(G_TIME_SPAN_SECOND * HOTP_RATE_LIMIT_IN_SEC));

    if (app_data->use_secret_service == TRUE) {
        gchar *pwd = secret_password_lookup_sync (OTPCLIENT_SCHEMA, NULL, NULL, "string", "main_pwd", NULL);
        if (pwd == NULL) {
            g_printerr ("%s\n", _("Couldn't find the password in the secret service."));
            goto retry;
        } else {
            app_data->db_data->key_stored = TRUE;
            app_data->db_data->key= secure_strdup (pwd);
            secret_password_free (pwd);
        }
    } else {
        retry:
        app_data->db_data->key = prompt_for_password (app_data, NULL, NULL, FALSE);
        if (app_data->db_data->key == NULL) {
            if (change_file (app_data) == FALSE) {
                g_free (app_data->db_data);
                g_free (app_data);
                g_application_quit (G_APPLICATION(app));
                return;
            }
        }
    }

    GError *err = NULL;
    load_db (app_data->db_data, &err);
    if (err != NULL && !g_error_matches (err, missing_file_gquark (), MISSING_FILE_CODE)) {
        show_message_dialog (app_data->main_window, err->message, GTK_MESSAGE_ERROR);
        gcry_free (app_data->db_data->key);
        if (g_error_matches (err, memlock_error_gquark (), MEMLOCK_ERRCODE)) {
            g_free (app_data->db_data);
            g_free (app_data);
            g_clear_error (&err);
            g_application_quit (G_APPLICATION(app));
            return;
        }
        g_clear_error (&err);
        goto retry;
    }

    if (app_data->use_secret_service == TRUE && app_data->db_data->key_stored == FALSE) {
        secret_password_store (OTPCLIENT_SCHEMA, SECRET_COLLECTION_DEFAULT, "main_pwd", app_data->db_data->key, NULL, on_password_stored, NULL, "string", "main_pwd", NULL);
    }

    if (g_error_matches (err, missing_file_gquark(), MISSING_FILE_CODE)) {
        const gchar *msg = _("This is the first time you run OTPClient, so you need to <b>add</b> or <b>import</b> some tokens.\n"
        "- to <b>add</b> tokens, please click the + button on the <b>top left</b>.\n"
        "- to <b>import</b> existing tokens, please click the menu button <b>on the top right</b>.\n"
        "\nIf you need more info, please visit the <a href=\"https://github.com/paolostivanin/OTPClient/wiki\">project's wiki</a>");
        show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_INFO);
        GError *tmp_err = NULL;
        update_and_reload_db (app_data, app_data->db_data, FALSE, &tmp_err);
        g_clear_error (&tmp_err);
    }

    app_data->clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);

    create_treeview (app_data);
    setup_kb_shortcuts (app_data);

    app_data->notification = g_notification_new ("OTPClient");
    g_notification_set_priority (app_data->notification, G_NOTIFICATION_PRIORITY_NORMAL);
    GIcon *icon = g_themed_icon_new ("com.github.paolostivanin.OTPClient");
    g_notification_set_icon (app_data->notification, icon);
    g_notification_set_body (app_data->notification, _("OTP value has been copied to the clipboard"));
    g_object_unref (icon);

    GtkToggleButton *reorder_toggle_btn = GTK_TOGGLE_BUTTON(gtk_builder_get_object (app_data->builder, "reorder_toggle_btn_id"));
    g_signal_connect (app_data->main_window, "toggle-reorder-button", G_CALLBACK(toggle_button_cb), reorder_toggle_btn);
    g_signal_connect (reorder_toggle_btn, "toggled", G_CALLBACK(reorder_rows_cb), app_data);
    g_signal_connect (app_data->main_window, "key_press_event", G_CALLBACK(key_pressed_cb), NULL);

    GtkToggleButton *del_toggle_btn = GTK_TOGGLE_BUTTON(gtk_builder_get_object (app_data->builder, "del_toggle_btn_id"));
    g_signal_connect (app_data->main_window, "toggle-delete-button", G_CALLBACK(toggle_button_cb), del_toggle_btn);
    g_signal_connect (del_toggle_btn, "toggled", G_CALLBACK(del_data_cb), app_data);
    g_signal_connect (app_data->main_window, "key_press_event", G_CALLBACK(key_pressed_cb), NULL);

    g_signal_connect (app_data->main_window, "destroy", G_CALLBACK(destroy_cb), app_data);

    app_data->source_id = g_timeout_add_full (G_PRIORITY_DEFAULT, 1000, traverse_liststore, app_data, NULL);

    setup_dbus_listener (app_data);

    // set last user activity to now, so we have a starting point for the autolock feature
    app_data->last_user_activity = g_date_time_new_now_local ();
    app_data->source_id_last_activity = g_timeout_add_seconds (1, check_inactivity, app_data);

    gtk_widget_show_all (app_data->main_window);

    app_data->info_bar = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "info_bar_id"));
    if (show_upgrade_msg ()) {
        set_info_bar (app_data, _("Not asking for password? Please check the 'Secret Service Integration' new feature <a href=\"https://github.com/paolostivanin/OTPClient/wiki/How-to-use-OTPClient#secret-service-integration\">HERE</a>"));
    } else {
        gtk_widget_hide (app_data->info_bar);
    }
}


static gboolean
show_memlock_warn_dialog (gint32      max_file_size,
                          GtkBuilder *builder)
{
    gchar *msg = g_strdup_printf (_("Your OS's memlock limit (%d) may be too low for you. "
                                  "This could crash the program when importing data from 3rd party apps "
                                  "or when a certain amount of tokens is reached. "
                                  "Please have a look at the <a href=\"https://github.com/paolostivanin/OTPClient/wiki/Secure-Memory-Limitations\">secure memory wiki</a> page before "
                                  "using this software with the current settings."), max_file_size);
    GtkWidget *warn_diag = GTK_WIDGET(gtk_builder_get_object (builder, "warning_diag_id"));
    GtkLabel *warn_label = GTK_LABEL(gtk_builder_get_object (builder, "warning_diag_label_id"));
    GtkWidget *warn_chk_btn = GTK_WIDGET(gtk_builder_get_object (builder, "warning_diag_check_btn_id"));
    gtk_label_set_label (warn_label, msg);
    gtk_widget_show_all (warn_diag);
    gboolean quit = FALSE;
    gint result = gtk_dialog_run (GTK_DIALOG (warn_diag));
    switch (result) {
        case GTK_RESPONSE_OK:
            set_warn_data (!gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON(warn_chk_btn)));
            break;
        case GTK_RESPONSE_CLOSE:
        default:
            set_warn_data (!gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON(warn_chk_btn)));
            quit = TRUE;
            break;
    }
    gtk_widget_destroy (warn_diag);
    g_free (msg);

    return quit;
}


static gboolean
key_pressed_cb (GtkWidget   *window,
                GdkEventKey *event_key,
                gpointer     user_data __attribute__((unused)))
{
    switch (event_key->keyval) {
        case GDK_KEY_q:
        if (event_key->state & GDK_CONTROL_MASK) {
            gtk_window_close (GTK_WINDOW(window));
        }
        break;
    }
    return FALSE;
}


static void
set_config_data (gint     *width,
                 gint     *height,
                 AppData  *app_data)
{
    GKeyFile *kf = get_kf_ptr ();
    GError *err = NULL;
    gboolean tmp;
    if (kf != NULL) {
        *width = g_key_file_get_integer (kf, "config", "window_width", NULL);
        *height = g_key_file_get_integer (kf, "config", "window_height", NULL);
        app_data->show_next_otp = g_key_file_get_boolean (kf, "config", "show_next_otp", NULL);
        app_data->disable_notifications = g_key_file_get_boolean (kf, "config", "notifications", NULL);
        app_data->search_column = g_key_file_get_integer (kf, "config", "search_column", NULL);
        app_data->auto_lock = g_key_file_get_boolean (kf, "config", "auto_lock", NULL);
        app_data->inactivity_timeout = g_key_file_get_integer (kf, "config", "inactivity_timeout", NULL);
        app_data->use_dark_theme = g_key_file_get_boolean (kf, "config", "dark_theme", NULL);
        // handle migration from disable_secret_service to use_secret_service
        tmp = g_key_file_get_boolean (kf, "config", "disable_secret_service", &err);
        if (tmp == TRUE || (tmp == FALSE && err == NULL)) {
            // old key was found, so we need to migrate to the new format
            migrate_secretservice_kf (app_data, kf, !tmp);
        }
        if (tmp == FALSE && err != NULL) {
            // key was not found, so we already migrated to the new format
            app_data->use_secret_service = g_key_file_get_boolean (kf, "config", "use_secret_service", NULL);
        }
        // end migration
        g_object_set (gtk_settings_get_default (), "gtk-application-prefer-dark-theme", app_data->use_dark_theme, NULL);
        g_key_file_free (kf);
    }
}


static void
migrate_secretservice_kf (AppData  *app_data,
                          GKeyFile *kf,
                          gboolean  value)
{
    GError *err = NULL;
    app_data->use_secret_service = value;
    g_key_file_set_boolean (kf, "config", "use_secret_service", app_data->use_secret_service);
    g_key_file_remove_key (kf, "config", "disable_secret_service", NULL);
    gchar *cfg_file_path;
#ifndef USE_FLATPAK_APP_FOLDER
    cfg_file_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
#else
    cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#endif
    if (!g_key_file_save_to_file (kf, cfg_file_path, &err)) {
        gchar *err_msg = g_strconcat (_("Couldn't save the config file: "), err->message, NULL);
        show_message_dialog (app_data->main_window, err_msg, GTK_MESSAGE_ERROR);
        g_free (err_msg);
        g_clear_error (&err);
    }
}

static gboolean
get_warn_data (void)
{
    GKeyFile *kf = get_kf_ptr ();
    gboolean show_warning = TRUE;
    GError *err = NULL;
    if (kf != NULL) {
        show_warning = g_key_file_get_boolean (kf, "config", "show_memlock_warning", &err);
        if (err != NULL && (err->code == G_KEY_FILE_ERROR_KEY_NOT_FOUND || err->code == G_KEY_FILE_ERROR_INVALID_VALUE)) {
            // value is not present, so we want to show the warning
            show_warning = TRUE;
        }
        g_key_file_free (kf);
    }

    return show_warning;
}


static void
set_warn_data (gboolean show_warning)
{
    GKeyFile *kf = get_kf_ptr ();
    GError *err = NULL;
    if (kf != NULL) {
        g_key_file_set_boolean (kf, "config", "show_memlock_warning", show_warning);
        gchar *cfg_file_path;
#ifndef USE_FLATPAK_APP_FOLDER
        cfg_file_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
#else
        cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#endif
        if (!g_key_file_save_to_file (kf, cfg_file_path, &err)) {
            g_printerr ("%s\n", err->message);
            g_clear_error (&err);
        }
        g_free (cfg_file_path);
        g_key_file_free (kf);
    }
}


static void
create_main_window (gint             width,
                    gint             height,
                    AppData         *app_data)
{
    app_data->main_window = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "appwindow_id"));
    gtk_window_set_icon_name (GTK_WINDOW(app_data->main_window), "otpclient");

    gtk_window_set_default_size (GTK_WINDOW(app_data->main_window), (width >= 150) ? width : 500, (height >= 150) ? height : 300);

    GtkWidget *lock_btn = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "lock_btn_id"));
    g_signal_connect (lock_btn, "clicked", G_CALLBACK(lock_app), app_data);
    if (app_data->use_secret_service == TRUE) {
        // secret service is enabled, so we can't lock the app
        gtk_widget_set_sensitive (lock_btn, FALSE);
    }

    set_action_group (app_data->builder, app_data);
}


static gboolean
show_upgrade_msg (void)
{
    gboolean show_msg = TRUE;
    GKeyFile *kf = get_kf_ptr ();
    if (kf != NULL) {
        gchar *up_msg = g_key_file_get_string (kf, "config", "upgrade_msg", NULL);
        if (up_msg == NULL) {
            show_msg = TRUE;
        } else {
            show_msg = (g_strcmp0 (up_msg, "v2_6") == 0) ? FALSE : TRUE;
        }
    }

    g_key_file_free (kf);

    return show_msg;
}


static void
set_info_bar (AppData     *app_data,
              const gchar *msg)
{
    GtkWidget *label = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "info_bar_label_id"));

    g_signal_connect (app_data->info_bar, "response", G_CALLBACK(on_bar_response), NULL);

    gtk_label_set_markup (GTK_LABEL(label), msg);
    gtk_info_bar_set_message_type (GTK_INFO_BAR(app_data->info_bar), GTK_MESSAGE_INFO);
    gtk_widget_show (app_data->info_bar);
}


static void
on_bar_response (GtkInfoBar *ib,
                 gint        response_id __attribute__((unused)),
                 gpointer    user_data   __attribute__((unused)))
{
    GError *err = NULL;
    GKeyFile *kf = get_kf_ptr ();
    if (kf != NULL) {
        g_key_file_set_string (kf, "config", "upgrade_msg", "v2_6");
        gchar *cfg_file_path;
#ifndef USE_FLATPAK_APP_FOLDER
        cfg_file_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
#else
        cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#endif
        if (!g_key_file_save_to_file (kf, cfg_file_path, &err)) {
            g_printerr ("%s\n", err->message);
            g_clear_error (&err);
        }
        g_free (cfg_file_path);
    }

    g_key_file_free (kf);

    gtk_widget_hide (GTK_WIDGET(ib));
}


static gboolean
set_action_group (GtkBuilder *builder,
                  AppData    *app_data)
{
    static GActionEntry settings_menu_entries[] = {
            { .name = ANDOTP_IMPORT_ACTION_NAME, .activate = select_file_cb },
            { .name = ANDOTP_IMPORT_PLAIN_ACTION_NAME, .activate = select_file_cb },
            { .name = FREEOTPPLUS_IMPORT_ACTION_NAME, .activate = select_file_cb },
            { .name = AEGIS_IMPORT_ACTION_NAME, .activate = select_file_cb },
            { .name = AEGIS_IMPORT_ENC_ACTION_NAME, .activate = select_file_cb },
            { .name = ANDOTP_EXPORT_ACTION_NAME, .activate = export_data_cb },
            { .name = ANDOTP_EXPORT_PLAIN_ACTION_NAME, .activate = export_data_cb },
            { .name = FREEOTPPLUS_EXPORT_ACTION_NAME, .activate = export_data_cb },
            { .name = AEGIS_EXPORT_ACTION_NAME, .activate = export_data_cb },
            { .name = AEGIS_EXPORT_PLAIN_ACTION_NAME, .activate = export_data_cb },
            { .name = GOOGLE_MIGRATION_FILE_ACTION_NAME, .activate = add_qr_from_file },
            { .name = GOOGLE_MIGRATION_WEBCAM_ACTION_NAME, .activate = webcam_add_cb },
            { .name = "create_newdb", .activate = new_db_cb },
            { .name = "change_db", .activate = change_db_cb },
            { .name = "change_pwd", .activate = change_password_cb },
            { .name = "edit_row", .activate = edit_row_cb },
            { .name = "show_qr", .activate = show_qr_cb },
            { .name = "settings", .activate = settings_dialog_cb },
            { .name = "shortcuts", .activate = shortcuts_window_cb },
            { .name = "dbinfo", .activate = dbinfo_cb },
            { .name = "about", .activate = about_diag_cb }
    };

    static GActionEntry add_menu_entries[] = {
            { .name = "webcam", .activate = webcam_add_cb },
            { .name = "import_qr_file", .activate = add_qr_from_file },
            { .name = "import_qr_clipboard", .activate = add_qr_from_clipboard },
            { .name = "manual", .activate = manual_add_cb }
    };

    GtkWidget *settings_popover = GTK_WIDGET (gtk_builder_get_object (builder, "settings_pop_id"));
    GActionGroup *settings_actions = (GActionGroup *)g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (settings_actions), settings_menu_entries, G_N_ELEMENTS (settings_menu_entries), app_data);
    gtk_widget_insert_action_group (settings_popover, "settings_menu", settings_actions);

    GtkWidget *add_popover = GTK_WIDGET (gtk_builder_get_object (builder, "add_pop_id"));
    GActionGroup *add_actions = (GActionGroup *)g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (add_actions), add_menu_entries, G_N_ELEMENTS (add_menu_entries), app_data);
    gtk_widget_insert_action_group (add_popover, "add_menu", add_actions);

    gtk_popover_set_constrain_to (GTK_POPOVER(add_popover), GTK_POPOVER_CONSTRAINT_NONE);
    gtk_popover_set_constrain_to (GTK_POPOVER(settings_popover), GTK_POPOVER_CONSTRAINT_NONE);

    return TRUE;
}


#ifndef USE_FLATPAK_APP_FOLDER
static gchar *
get_db_path (AppData *app_data)
{
    gchar *db_path = NULL;
    GError *err = NULL;
    GKeyFile *kf = g_key_file_new ();
    gchar *cfg_file_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
    if (g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
        if (!g_key_file_load_from_file (kf, cfg_file_path, G_KEY_FILE_NONE, &err)) {
            show_message_dialog (app_data->main_window, err->message, GTK_MESSAGE_ERROR);
            g_key_file_free (kf);
            g_clear_error (&err);
            return NULL;
        }
        db_path = g_key_file_get_string (kf, "config", "db_path", NULL);
        if (db_path == NULL) {
            goto new_db;
        }
        if (!g_file_test (db_path, G_FILE_TEST_EXISTS)) {
            gchar *msg = g_strconcat ("Database file/location:\n<b>", db_path, "</b>\ndoes not exist. A new database will be created.", NULL);
            show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
            g_free (msg);
            goto new_db;
        }
        goto end;
    }
    new_db: ; // empty statement workaround
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new (_("Select database location"),
                                                                GTK_WINDOW(app_data->main_window),
                                                                app_data->open_db_file_action,
                                                                "OK",
                                                                "Cancel");

    GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
    gtk_file_chooser_set_do_overwrite_confirmation (chooser, TRUE);
    gtk_file_chooser_set_select_multiple (chooser, FALSE);
    if (app_data->open_db_file_action == GTK_FILE_CHOOSER_ACTION_SAVE) {
        gtk_file_chooser_set_current_name (chooser, "NewDatabase.enc");
    }

    gint res = gtk_native_dialog_run (GTK_NATIVE_DIALOG(dialog));

    if (res == GTK_RESPONSE_ACCEPT) {
        db_path = gtk_file_chooser_get_filename (chooser);
        g_key_file_set_string (kf, "config", "db_path", db_path);
        if (!g_key_file_save_to_file (kf, cfg_file_path, &err)) {
            g_printerr ("%s\n", err->message);
            g_clear_error (&err);
        }
    }

    // clear any password that may have been previously set, thus avoiding using a wrong password with a new database
    secret_password_clear (OTPCLIENT_SCHEMA, NULL, on_password_cleared, NULL, "string", "main_pwd", NULL);

    g_object_unref (dialog);

    end:
    g_free (cfg_file_path);
    g_key_file_free (kf);

    return db_path;
}
#endif


static void
toggle_button_cb (GtkWidget *main_window __attribute__((unused)),
                  gpointer   user_data)
{
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON(user_data), !gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON(user_data)));
}


static void
reorder_rows_cb (GtkToggleButton *btn,
                 gpointer         user_data)
{
    AppData *app_data = (AppData *)user_data;
    gboolean is_btn_active = gtk_toggle_button_get_active (btn);
    gtk_tree_view_set_reorderable (GTK_TREE_VIEW(app_data->tree_view), is_btn_active);
    app_data->is_reorder_active = is_btn_active;
    gtk_widget_set_sensitive (GTK_WIDGET(gtk_builder_get_object (app_data->builder, "add_btn_main_id")), !is_btn_active);
    gtk_widget_set_sensitive (GTK_WIDGET(gtk_builder_get_object (app_data->builder, "del_toggle_btn_id")), !is_btn_active);

    if (is_btn_active == FALSE) {
        // reordering has been disabled, so now we have to reorder and update the database itself
        reorder_db (app_data);
    }
}


static void
del_data_cb (GtkToggleButton *btn,
             gpointer         user_data)
{
    AppData *app_data = (AppData *)user_data;

    GtkStyleContext *gsc_btn = gtk_widget_get_style_context (GTK_WIDGET(btn));
    GtkStyleContext *gsc_tv = gtk_widget_get_style_context (GTK_WIDGET(app_data->tree_view));

    GtkTreeSelection *tree_selection = gtk_tree_view_get_selection (app_data->tree_view);

    if (gtk_toggle_button_get_active (btn)) {
        app_data->delbtn_css_provider = gtk_css_provider_new ();
        app_data->tv_css_provider = gtk_css_provider_new ();

        gtk_css_provider_load_from_data (app_data->delbtn_css_provider, "#delbtn { background: #970000; }", -1, NULL);
        gtk_css_provider_load_from_data (app_data->tv_css_provider, "#tv { background: #970000; }", -1, NULL);

        gtk_style_context_add_provider (gsc_btn, GTK_STYLE_PROVIDER(app_data->delbtn_css_provider), GTK_STYLE_PROVIDER_PRIORITY_USER);
        gtk_style_context_add_provider (gsc_tv, GTK_STYLE_PROVIDER(app_data->tv_css_provider), GTK_STYLE_PROVIDER_PRIORITY_USER);

        const gchar *msg = _("You just entered the deletion mode. You can now click on the row(s) you'd like to delete.\n"
            "Please note that once a row has been deleted, <b>it's impossible to recover the associated data.</b>");

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
        gtk_style_context_remove_provider (gsc_btn, GTK_STYLE_PROVIDER(app_data->delbtn_css_provider));
        gtk_style_context_remove_provider (gsc_tv, GTK_STYLE_PROVIDER(app_data->tv_css_provider));
        g_object_unref (app_data->delbtn_css_provider);
        g_object_unref (app_data->tv_css_provider);
        g_signal_handlers_disconnect_by_func (app_data->tree_view, delete_rows_cb, app_data);
        g_signal_connect (app_data->tree_view, "row-activated", G_CALLBACK(row_selected_cb), app_data);
    }
}


static void
get_window_size_cb (GtkWidget      *window,
                    GtkAllocation  *allocation __attribute__((unused)),
                    gpointer        user_data  __attribute__((unused)))
{
    gint w, h;
    gtk_window_get_size (GTK_WINDOW(window), &w, &h);
    g_object_set_data (G_OBJECT(window), "width", GINT_TO_POINTER(w));
    g_object_set_data (G_OBJECT(window), "height", GINT_TO_POINTER(h));
}


void
destroy_cb (GtkWidget   *window,
            gpointer     user_data)
{
    AppData *app_data = (AppData *)user_data;
    save_sort_order (app_data->tree_view);
    g_source_remove (app_data->source_id);
    g_source_remove (app_data->source_id_last_activity);
    g_date_time_unref (app_data->last_user_activity);
    for (gint i = 0; i < DBUS_SERVICES; i++) {
        g_dbus_connection_signal_unsubscribe (app_data->connection, app_data->subscription_ids[i]);
    }
    g_dbus_connection_close (app_data->connection, NULL, NULL, NULL);
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
    gint w = GPOINTER_TO_INT(g_object_get_data (G_OBJECT(window), "width"));
    gint h = GPOINTER_TO_INT(g_object_get_data (G_OBJECT(window), "height"));
#pragma GCC diagnostic pop
    save_window_size (w, h);
    g_object_unref (app_data->builder);
    g_free (app_data);
    gcry_control (GCRYCTL_TERM_SECMEM);
}


static void
save_sort_order (GtkTreeView *tree_view)
{
    gint id;
    GtkSortType order;
    gtk_tree_sortable_get_sort_column_id (GTK_TREE_SORTABLE(GTK_LIST_STORE(gtk_tree_view_get_model (tree_view))), &id, &order);
    // store data only if it was changed
    if (id >= 0) {
        store_data ("column_id", id, "sort_order", order);
    }
}


static void
save_window_size (gint width,
                  gint height)
{
    store_data ("window_width", width, "window_height", height);
}


static void
store_data (const gchar *param1_name,
            gint         param1_value,
            const gchar *param2_name,
            gint         param2_value)
{
    GError *err = NULL;
    GKeyFile *kf = g_key_file_new ();
    gchar *cfg_file_path;
#ifndef USE_FLATPAK_APP_FOLDER
    cfg_file_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
#else
    cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#endif
    if (g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
        if (!g_key_file_load_from_file (kf, cfg_file_path, G_KEY_FILE_NONE, &err)) {
            g_printerr ("%s\n", err->message);
            g_clear_error (&err);
        } else {
            g_key_file_set_integer (kf, "config", param1_name, param1_value);
            g_key_file_set_integer (kf, "config", param2_name, param2_value);
            if (!g_key_file_save_to_file (kf, cfg_file_path, &err)) {
                g_printerr ("%s\n", err->message);
                g_clear_error (&err);
            }
        }
    }
    g_key_file_free (kf);
    g_free (cfg_file_path);
}


static void
set_open_db_action (GtkWidget *btn,
                    gpointer   user_data)
{
    AppData *app_data = (AppData *)user_data;
    app_data->open_db_file_action = g_strcmp0 (gtk_widget_get_name (btn), "diag_rc_restoredb_btn") == 0 ? GTK_FILE_CHOOSER_ACTION_OPEN : GTK_FILE_CHOOSER_ACTION_SAVE;
    gtk_dialog_response (GTK_DIALOG(app_data->diag_rcdb), GTK_RESPONSE_OK);
}
