#include <gtk/gtk.h>
#include <gcrypt.h>
#include <jansson.h>
#include <libsecret/secret.h>
#include <glib/gi18n.h>
#include "otpclient-application.h"
#include "otpclient-window.h"
#include "otpclient.h"
#include "../common/gquarks.h"
#include "gui-misc.h"
#include "../common/import-export.h"
#include "message-dialogs.h"
#include "password-cb.h"
#include "get-builder.h"
#include "liststore-misc.h"
#include "lock-app.h"
#include "change-db-cb.h"
#include "new-db-cb.h"
#include "../common/secret-schema.h"
#include "../common/macros.h"
#include "change-pwd-cb.h"
#include "settings-cb.h"
#include "setup-signals-shortcuts.h"
#include "shortcuts-cb.h"
#include "webcam-add-cb.h"
#include "manual-add-cb.h"
#include "dbinfo-cb.h"
#include "change-file-cb.h"
#include "change-db-sec.h"
#include "../common/file-size.h"
#include "../common/common.h"
#ifdef ENABLE_MINIMIZE_TO_TRAY
#include "tray.h"
#endif

#ifndef IS_FLATPAK
static gchar     *get_db_path               (AppData            *app_data);
#endif

static void       set_config_data           (gint               *width,
                                             gint               *height,
                                             AppData            *app_data);

static void       set_open_db_action        (GtkWidget          *btn,
                                             gpointer            user_data);

static void       init_app_defaults         (AppData            *app_data);

static void       init_db_defaults          (AppData            *app_data);

static void       cleanup_app_data          (AppData            *app_data);

static void       otpclient_application_shutdown (GApplication *app);

static void       load_validity_colors      (GKeyFile           *kf,
                                             AppData            *app_data);

struct _OtpclientApplication {
    GtkApplication parent_instance;
    AppData *app_data;
};

struct _OtpclientApplicationClass {
    GtkApplicationClass parent_class;
};

G_DEFINE_TYPE (OtpclientApplication, otpclient_application, GTK_TYPE_APPLICATION)

static void
otpclient_application_activate (GApplication *app)
{
    OtpclientApplication *self = OTPCLIENT_APPLICATION (app);
    if (self->app_data != NULL) {
        gtk_window_present (GTK_WINDOW (self->app_data->main_window));
        return;
    }

    gint32 memlock_value = 0;
    gint32 memlock_ret_value = set_memlock_value (&memlock_value);

    AppData *app_data = g_new0 (AppData, 1);
    init_app_defaults (app_data);
    g_type_ensure (OTPCLIENT_TYPE_WINDOW);
    app_data->builder = get_builder_from_partial_path (UI_PARTIAL_PATH);
    app_data->add_popover_builder = get_builder_from_partial_path (AP_PARTIAL_PATH);
    app_data->settings_popover_builder = get_builder_from_partial_path (SP_PARTIAL_PATH);

    app_data->db_data = g_new0 (DatabaseData, 1);
    init_db_defaults (app_data);

    gint width = 0;
    gint height = 0;
    set_config_data (&width, &height, app_data);

    OtpclientWindow *window = otpclient_window_new (GTK_APPLICATION (app), width, height, app_data);
    if (window == NULL) {
        g_printerr ("%s\n", _("Couldn't locate the ui file, exiting..."));
        cleanup_app_data (app_data);
        g_application_quit (app);
        return;
    }

    if (memlock_ret_value == MEMLOCK_ERR) {
        gchar *msg = g_strdup_printf (_("Couldn't get the memlock value, therefore secure memory cannot be allocated. Please have a look at the"
                                        "<a href=\"https://github.com/paolostivanin/OTPClient/wiki/Secure-Memory-Limitations\">secure memory</a> wiki page before re-running OTPClient."));
        show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
        g_free (msg);
        cleanup_app_data (app_data);
        g_application_quit (app);
        return;
    }

    gchar *init_msg = init_libs (memlock_value);
    if (init_msg != NULL) {
        show_message_dialog (app_data->main_window, init_msg, GTK_MESSAGE_ERROR);
        g_free (init_msg);
        cleanup_app_data (app_data);
        g_application_quit (app);
        return;
    }

#ifdef IS_FLATPAK
    // Check if a path is already set in the config
    GKeyFile *kf = get_kf_ptr ();
    if (kf != NULL) {
        gchar *db_path = g_key_file_get_string (kf, "config", "db_path", NULL);
        if (db_path != NULL) {
            app_data->db_data->db_path = db_path;
        } else {
            // Use the default path only if no path is set in config
            app_data->db_data->db_path = g_build_filename (g_get_user_data_dir (), "otpclient-db.enc", NULL);
            gchar *cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
            if (g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
                g_key_file_set_string (kf, "config", "db_path", app_data->db_data->db_path);
                g_key_file_save_to_file (kf, cfg_file_path, NULL);
            }
            g_free (cfg_file_path);
        }
        g_key_file_free (kf);
    } else {
        // If no config exists yet, use the default path
        app_data->db_data->db_path = g_build_filename (g_get_user_data_dir (), "otpclient-db.enc", NULL);
        // Create a minimal config
        gchar *cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
        if (!g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
            GKeyFile *new_kf = g_key_file_new ();
            g_key_file_set_string (new_kf, "config", "db_path", app_data->db_data->db_path);
            g_key_file_save_to_file (new_kf, cfg_file_path, NULL);
            g_key_file_free (new_kf);
        }
        g_free (cfg_file_path);
    }
#else
    if (!g_file_test (g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL), G_FILE_TEST_EXISTS)) {
        app_data->diag_rcdb = GTK_WIDGET (gtk_builder_get_object (app_data->builder, "dialog_rcdb_id"));
        GtkWidget *restore_btn = GTK_WIDGET (gtk_builder_get_object (app_data->builder, "diag_rc_restoredb_btn_id"));
        GtkWidget *create_btn = GTK_WIDGET (gtk_builder_get_object (app_data->builder, "diag_rc_createdb_btn_id"));
        g_signal_connect (restore_btn, "clicked", G_CALLBACK (set_open_db_action), app_data);
        g_signal_connect (create_btn, "clicked", G_CALLBACK (set_open_db_action), app_data);

        gint response = gtk_dialog_run (GTK_DIALOG (app_data->diag_rcdb));
        switch (response) {
            case GTK_RESPONSE_CANCEL:
            default:
                gtk_widget_destroy (app_data->diag_rcdb);
                cleanup_app_data (app_data);
                g_application_quit (app);
                return;
            case GTK_RESPONSE_OK:
                gtk_widget_destroy (app_data->diag_rcdb);
        }
    }

    app_data->db_data->db_path = get_db_path (app_data);
    if (app_data->db_data->db_path == NULL) {
        cleanup_app_data (app_data);
        g_application_quit (app);
        return;
    }
#endif

    app_data->db_data->max_file_size_from_memlock = memlock_value;
    app_data->db_data->objects_hash = NULL;
    app_data->db_data->data_to_add = NULL;
    // subtract 3 seconds from the current time. Needed for "last_hotp" to be set on the first run
    app_data->db_data->last_hotp_update = g_date_time_add_seconds (g_date_time_new_now_local (), -(G_TIME_SPAN_SECOND * HOTP_RATE_LIMIT_IN_SEC));

    if (app_data->use_secret_service == TRUE) {
        gchar *pwd = secret_password_lookup_sync (OTPCLIENT_SCHEMA, NULL, NULL, "string", "main_pwd", NULL);
        if (pwd == NULL) {
            g_printerr ("%s\n", _("Couldn't find the password in the secret service."));
            goto retry;
        }
        app_data->db_data->key_stored = TRUE;
        app_data->db_data->key = secure_strdup (pwd);
        secret_password_free (pwd);
    } else {
        retry:
        app_data->db_data->key = prompt_for_password (app_data, NULL, NULL, FALSE);
        if (app_data->db_data->key == NULL) {
            retry_change_file:
            if (change_file (app_data) == QUIT_APP) {
                cleanup_app_data (app_data);
                g_application_quit (app);
                return;
            }
            goto retry_change_file;
        }
    }

    if (get_file_size (app_data->db_data->db_path) > (goffset) (app_data->db_data->max_file_size_from_memlock * SECMEM_SIZE_THRESHOLD_RATIO)) {
        gchar *msg = g_strdup_printf (_(
            "Your system's secure memory limit (memlock: %d bytes) is not enough to securely load the database into memory.\n"
            "You need to increase your system's memlock limit by following the instructions on our "
            "<a href=\"https://github.com/paolostivanin/OTPClient/wiki/Secure-Memory-Limitations\">secure memory wiki page</a>.\n"
            "This requires administrator privileges and is a system-wide setting that OTPClient cannot change automatically."
        ), memlock_value);
        g_printerr ("%s\n", msg);
        g_free (msg);
        cleanup_app_data (app_data);
        g_application_quit (app);
        return;
    }

    GError *err = NULL;
    load_db (app_data->db_data, &err);
    if (err != NULL && !g_error_matches (err, missing_file_gquark (), MISSING_FILE_ERRCODE)) {
        show_message_dialog (app_data->main_window, err->message, GTK_MESSAGE_ERROR);
        g_clear_pointer (&app_data->db_data->key, gcry_free);
        if (g_error_matches (err, memlock_error_gquark (), MEMLOCK_ERRCODE)) {
            cleanup_app_data (app_data);
            g_clear_error (&err);
            g_application_quit (app);
            return;
        }
        g_clear_error (&err);
        goto retry;
    }

    if (app_data->use_secret_service == TRUE && app_data->db_data->key_stored == FALSE) {
        secret_password_store (OTPCLIENT_SCHEMA, SECRET_COLLECTION_DEFAULT, "main_pwd", app_data->db_data->key, NULL, on_password_stored, NULL, "string", "main_pwd", NULL);
    }

    if (g_error_matches (err, missing_file_gquark(), MISSING_FILE_ERRCODE)) {
        const gchar *msg = _("This is the first time you run OTPClient, so you need to <b>add</b> or <b>import</b> some tokens.\n"
        "- to <b>add</b> tokens, please click the + button on the <b>top left</b>.\n"
        "- to <b>import</b> existing tokens, please click the menu button <b>on the top right</b>.\n"
        "\nIf you need more info, please visit the <a href=\"https://github.com/paolostivanin/OTPClient/wiki\">project's wiki</a>");
        show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_INFO);
        GError *tmp_err = NULL;
        update_db (app_data->db_data, &tmp_err);
        reload_db (app_data->db_data, &tmp_err);
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

    app_data->source_id = g_timeout_add_full (G_PRIORITY_DEFAULT, 1000, traverse_liststore, app_data, NULL);

    setup_dbus_listener (app_data);

    // set last user activity to now, so we have a starting point for the autolock feature
    app_data->last_user_activity = g_date_time_new_now_local ();
    app_data->source_id_last_activity = g_timeout_add_seconds (1, check_inactivity, app_data);

    self->app_data = app_data;

    gtk_widget_show_all (app_data->main_window);
    gtk_widget_hide (app_data->search_entry);
}

static void
otpclient_application_finalize (GObject *object)
{
    OtpclientApplication *self = OTPCLIENT_APPLICATION (object);

    if (self->app_data != NULL) {
        cleanup_app_data (self->app_data);
        self->app_data = NULL;
    }

    G_OBJECT_CLASS (otpclient_application_parent_class)->finalize (object);
}

static void
otpclient_application_class_init (OtpclientApplicationClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    GApplicationClass *app_class = G_APPLICATION_CLASS (klass);

    object_class->finalize = otpclient_application_finalize;
    app_class->activate = otpclient_application_activate;
    app_class->shutdown = otpclient_application_shutdown;
}

static void
otpclient_application_init (OtpclientApplication *self)
{
    self->app_data = NULL;
}

OtpclientApplication *
otpclient_application_new (void)
{
    GApplicationFlags flags;
#if GLIB_CHECK_VERSION(2, 74, 0)
    flags = G_APPLICATION_DEFAULT_FLAGS;
#else
    flags = G_APPLICATION_FLAGS_NONE;
#endif
    return g_object_new (OTPCLIENT_TYPE_APPLICATION,
                         "application-id", "com.github.paolostivanin.OTPClient",
                         "flags", flags,
                         NULL);
}

void
otpclient_application_clear_app_data (OtpclientApplication *app)
{
    if (app == NULL) {
        return;
    }
    app->app_data = NULL;
}

static void
set_config_data (gint    *width,
                 gint    *height,
                 AppData *app_data)
{
    GKeyFile *kf = get_kf_ptr ();
    if (kf != NULL) {
        *width = g_key_file_get_integer (kf, "config", "window_width", NULL);
        *height = g_key_file_get_integer (kf, "config", "window_height", NULL);
        app_data->show_next_otp = g_key_file_get_boolean (kf, "config", "show_next_otp", NULL);
        app_data->disable_notifications = g_key_file_get_boolean (kf, "config", "notifications", NULL);
        app_data->show_validity_seconds = g_key_file_get_boolean (kf, "config", "show_validity_seconds", NULL);
        app_data->auto_lock = g_key_file_get_boolean (kf, "config", "auto_lock", NULL);
        app_data->inactivity_timeout = g_key_file_get_integer (kf, "config", "inactivity_timeout", NULL);
        app_data->use_dark_theme = g_key_file_get_boolean (kf, "config", "dark_theme", NULL);
        app_data->use_tray = g_key_file_get_boolean (kf, "config", "use_tray", NULL);
        app_data->use_secret_service = g_key_file_get_boolean (kf, "config", "use_secret_service", NULL);
        if (g_key_file_has_key (kf, "config", "search_provider_enabled", NULL)) {
            app_data->search_provider_enabled = g_key_file_get_boolean (kf, "config", "search_provider_enabled", NULL);
        } else {
            app_data->search_provider_enabled = TRUE;
        }
        load_validity_colors (kf, app_data);
        g_object_set (gtk_settings_get_default (), "gtk-application-prefer-dark-theme", app_data->use_dark_theme, NULL);
        g_key_file_free (kf);
    }
}

#ifndef IS_FLATPAK
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
            g_free (db_path);
            db_path = NULL;
            goto new_db;
        }
        goto end;
    }
    new_db: ; // empty statement workaround
    GtkFileChooserNative *dialog = gtk_file_chooser_native_new (_("Select database location"),
                                                                GTK_WINDOW (app_data->main_window),
                                                                app_data->open_db_file_action,
                                                                "OK",
                                                                "Cancel");

    GtkFileChooser *chooser = GTK_FILE_CHOOSER (dialog);
    gtk_file_chooser_set_do_overwrite_confirmation (chooser, TRUE);
    gtk_file_chooser_set_select_multiple (chooser, FALSE);
    if (app_data->open_db_file_action == GTK_FILE_CHOOSER_ACTION_SAVE) {
        gtk_file_chooser_set_current_name (chooser, "NewDatabase.enc");
    }

    gint res = gtk_native_dialog_run (GTK_NATIVE_DIALOG (dialog));

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
set_open_db_action (GtkWidget *btn,
                    gpointer   user_data)
{
    CAST_USER_DATA (AppData, app_data, user_data);
    app_data->open_db_file_action = g_strcmp0 (gtk_widget_get_name (btn), "diag_rc_restoredb_btn") == 0 ? GTK_FILE_CHOOSER_ACTION_OPEN : GTK_FILE_CHOOSER_ACTION_SAVE;
    gtk_dialog_response (GTK_DIALOG (app_data->diag_rcdb), GTK_RESPONSE_OK);
}

static void
init_app_defaults (AppData *app_data)
{
    app_data->app_locked = FALSE;
    app_data->show_next_otp = FALSE; // next otp not shown by default
    app_data->disable_notifications = FALSE; // notifications enabled by default
    app_data->show_validity_seconds = FALSE; // validity is shown as a pie chart by default
    gdk_rgba_parse (&app_data->validity_color, "#33A659");
    gdk_rgba_parse (&app_data->validity_warning_color, "#D95940");
    app_data->auto_lock = FALSE; // disabled by default
    app_data->inactivity_timeout = 0; // never
    app_data->use_dark_theme = FALSE; // light theme by default
    app_data->use_secret_service = TRUE; // secret service enabled by default
    app_data->is_reorder_active = FALSE; // when app is started, reorder is not set
    app_data->use_tray = FALSE; // do not use tray by default
    app_data->search_provider_enabled = TRUE; // search provider enabled by default
    // open_db_file_action is set only on first startup and not when the db is deleted but the cfg file is there, therefore we need a default action
    app_data->open_db_file_action = GTK_FILE_CHOOSER_ACTION_SAVE;
    app_data->window_width = 0;
    app_data->window_height = 0;
}

static void
load_validity_colors (GKeyFile *kf,
                      AppData  *app_data)
{
    if (kf == NULL || app_data == NULL) {
        return;
    }

    gchar *validity_color = g_key_file_get_string (kf, "config", "validity_color", NULL);
    if (validity_color != NULL) {
        gdk_rgba_parse (&app_data->validity_color, validity_color);
    }
    g_free (validity_color);

    gchar *warning_color = g_key_file_get_string (kf, "config", "validity_warning_color", NULL);
    if (warning_color != NULL) {
        gdk_rgba_parse (&app_data->validity_warning_color, warning_color);
    }
    g_free (warning_color);
}

static void
init_db_defaults (AppData *app_data)
{
    app_data->db_data->key_stored = FALSE; // at startup, we don't know whether the key is stored or not
    app_data->db_data->max_file_size_from_memlock = 0;
    app_data->db_data->objects_hash = NULL;
    app_data->db_data->data_to_add = NULL;
}

static void
cleanup_app_data (AppData *app_data)
{
    if (app_data == NULL) {
        return;
    }

    if (app_data->main_window != NULL) {
        gtk_widget_destroy (app_data->main_window);
    }

    if (app_data->builder != NULL) {
        g_object_unref (app_data->builder);
    }
    if (app_data->add_popover_builder != NULL) {
        g_object_unref (app_data->add_popover_builder);
    }
    if (app_data->settings_popover_builder != NULL) {
        g_object_unref (app_data->settings_popover_builder);
    }

    if (app_data->db_data != NULL) {
        g_clear_pointer (&app_data->db_data->db_path, g_free);
        g_clear_pointer (&app_data->db_data->key, gcry_free);
        g_free (app_data->db_data);
    }

    g_free (app_data);
}

static void
otpclient_application_shutdown (GApplication *app)
{
    OtpclientApplication *self = OTPCLIENT_APPLICATION (app);

    if (self->app_data != NULL) {
        destroy_cb (NULL, self->app_data);
    }

    G_APPLICATION_CLASS (otpclient_application_parent_class)->shutdown (app);
}
