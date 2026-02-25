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
static gchar   *get_db_path            (AppData            *app_data);
#endif

static void     set_config_data        (gint               *width,
                                        gint               *height,
                                        AppData            *app_data);

static void     set_open_db_action     (GtkWidget          *btn,
                                        gpointer            user_data);

static void     init_app_defaults      (AppData            *app_data);

static void     init_db_defaults       (AppData            *app_data);

static void     cleanup_app_data       (AppData            *app_data);

static void     otpclient_application_shutdown (GApplication *app);

static void     load_validity_colors   (GKeyFile           *kf,
                                        AppData            *app_data);

/* Activation helpers */
static gboolean resolve_db_path        (GApplication       *app,
                                        AppData            *app_data);

static gboolean load_db_with_password  (GApplication       *app,
                                        AppData            *app_data,
                                        gint32              memlock_value);

static void     setup_ui_and_timers    (GApplication       *app,
                                        AppData            *app_data);

struct _OtpclientApplication {
    GtkApplication  parent_instance;
    AppData        *app_data;
};

G_DEFINE_TYPE (OtpclientApplication, otpclient_application, GTK_TYPE_APPLICATION)

/* ── activate ──────────────────────────────────────────────────────────── */

static void
otpclient_application_activate (GApplication *app)
{
    OtpclientApplication *self = OTPCLIENT_APPLICATION (app);
    if (self->app_data != NULL) {
        gtk_window_present (GTK_WINDOW (self->app_data->main_window));
        return;
    }

    gint32 memlock_value    = 0;
    gint32 memlock_ret_value = set_memlock_value (&memlock_value);

    AppData *app_data = g_new0 (AppData, 1);
    init_app_defaults (app_data);

    g_type_ensure (OTPCLIENT_TYPE_WINDOW);

    app_data->db_data = g_new0 (DatabaseData, 1);
    init_db_defaults (app_data);

    gint width = 0, height = 0;
    set_config_data (&width, &height, app_data);

    OtpclientWindow *window = otpclient_window_new (GTK_APPLICATION (app),
                                                     width, height, app_data);
    if (window == NULL) {
        g_printerr ("%s\n", _("Couldn't locate the ui file, exiting..."));
        cleanup_app_data (app_data);
        g_application_quit (app);
        return;
    }

    if (memlock_ret_value == MEMLOCK_ERR) {
        gchar *msg = g_strdup_printf (_(
            "Couldn't get the memlock value, therefore secure memory cannot be allocated. "
            "Please have a look at the "
            "<a href=\"https://github.com/paolostivanin/OTPClient/wiki/Secure-Memory-Limitations\">"
            "secure memory</a> wiki page before re-running OTPClient."));
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

    if (!resolve_db_path (app, app_data)) {
        cleanup_app_data (app_data);
        g_application_quit (app);
        return;
    }

    app_data->db_data->max_file_size_from_memlock = memlock_value;
    app_data->db_data->objects_hash  = NULL;
    app_data->db_data->data_to_add   = NULL;
    /* Subtract 3 s so "last_hotp" is valid from the very first run */
    GDateTime *now = g_date_time_new_now_local ();
    app_data->db_data->last_hotp_update =
        g_date_time_add_seconds (now, -(G_TIME_SPAN_SECOND * HOTP_RATE_LIMIT_IN_SEC));
    g_date_time_unref (now);

    if (!load_db_with_password (app, app_data, memlock_value)) {
        cleanup_app_data (app_data);
        g_application_quit (app);
        return;
    }

    setup_ui_and_timers (app, app_data);

    self->app_data = app_data;

    gtk_widget_show_all (app_data->main_window);
    /* search_entry has no_show_all set; it stays hidden until Ctrl+F */
}

/* ── resolve_db_path ───────────────────────────────────────────────────── */

static gboolean
resolve_db_path (GApplication *app UNUSED,
                 AppData      *app_data)
{
#ifdef IS_FLATPAK
    GKeyFile *kf = get_kf_ptr ();
    if (kf != NULL) {
        gchar *db_path = g_key_file_get_string (kf, "config", "db_path", NULL);
        if (db_path != NULL) {
            app_data->db_data->db_path = db_path;
        } else {
            app_data->db_data->db_path =
                g_build_filename (g_get_user_data_dir (), "otpclient-db.enc", NULL);
            gchar *cfg_file_path =
                g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
            if (g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
                g_key_file_set_string (kf, "config", "db_path", app_data->db_data->db_path);
                g_key_file_save_to_file (kf, cfg_file_path, NULL);
            }
            g_free (cfg_file_path);
        }
        g_key_file_free (kf);
    } else {
        app_data->db_data->db_path =
            g_build_filename (g_get_user_data_dir (), "otpclient-db.enc", NULL);
        gchar *cfg_file_path =
            g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
        if (!g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
            GKeyFile *new_kf = g_key_file_new ();
            g_key_file_set_string (new_kf, "config", "db_path", app_data->db_data->db_path);
            g_key_file_save_to_file (new_kf, cfg_file_path, NULL);
            g_key_file_free (new_kf);
        }
        g_free (cfg_file_path);
    }
    return TRUE;
#else
    gchar *cfg_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
    gboolean cfg_exists = g_file_test (cfg_path, G_FILE_TEST_EXISTS);
    g_free (cfg_path);
    if (!cfg_exists) {
        app_data->diag_rcdb =
            GTK_WIDGET (gtk_builder_get_object (app_data->builder, "dialog_rcdb_id"));
        GtkWidget *restore_btn =
            GTK_WIDGET (gtk_builder_get_object (app_data->builder, "diag_rc_restoredb_btn_id"));
        GtkWidget *create_btn =
            GTK_WIDGET (gtk_builder_get_object (app_data->builder, "diag_rc_createdb_btn_id"));
        g_signal_connect (restore_btn, "clicked", G_CALLBACK (set_open_db_action), app_data);
        g_signal_connect (create_btn,  "clicked", G_CALLBACK (set_open_db_action), app_data);

        gint response = gtk_dialog_run (GTK_DIALOG (app_data->diag_rcdb));
        switch (response) {
            case GTK_RESPONSE_CANCEL:
            default:
                gtk_widget_destroy (app_data->diag_rcdb);
                return FALSE;
            case GTK_RESPONSE_OK:
                gtk_widget_destroy (app_data->diag_rcdb);
        }
    }

    app_data->db_data->db_path = get_db_path (app_data);
    return app_data->db_data->db_path != NULL;
#endif
}

/* ── load_db_with_password ─────────────────────────────────────────────── */

static gboolean
load_db_with_password (GApplication *app UNUSED,
                       AppData      *app_data,
                       gint32        memlock_value)
{
    if (get_file_size (app_data->db_data->db_path) >
            (goffset) (app_data->db_data->max_file_size_from_memlock * SECMEM_SIZE_THRESHOLD_RATIO)) {
        gchar *msg = g_strdup_printf (_(
            "Your system's secure memory limit (memlock: %d bytes) is not enough to securely "
            "load the database into memory.\n"
            "You need to increase your system's memlock limit by following the instructions on our "
            "<a href=\"https://github.com/paolostivanin/OTPClient/wiki/Secure-Memory-Limitations\">"
            "secure memory wiki page</a>.\n"
            "This requires administrator privileges and is a system-wide setting that OTPClient "
            "cannot change automatically."),
            memlock_value);
        g_printerr ("%s\n", msg);
        g_free (msg);
        return FALSE;
    }

    if (app_data->use_secret_service) {
        gchar *pwd = secret_password_lookup_sync (OTPCLIENT_SCHEMA, NULL, NULL,
                                                   "string", "main_pwd", NULL);
        if (pwd != NULL) {
            app_data->db_data->key_stored = TRUE;
            app_data->db_data->key = secure_strdup (pwd);
            secret_password_free (pwd);
        } else {
            g_printerr ("%s\n", _("Couldn't find the password in the secret service."));
            /* Fall through to manual password prompt */
            app_data->use_secret_service = FALSE;
        }
    }

    GError *err = NULL;

    if (!app_data->use_secret_service) {
        /* Prompt until the user provides a valid password or opts to change file */
        do {
            g_clear_pointer (&app_data->db_data->key, gcry_free);
            app_data->db_data->key = prompt_for_password (app_data, NULL, NULL, FALSE);

            if (app_data->db_data->key == NULL) {
                /* User cancelled — let them switch to a different database file */
                gint change_res;
                do {
                    change_res = change_file (app_data);
                } while (change_res != QUIT_APP && change_res != CHANGE_OK);

                if (change_res == QUIT_APP) {
                    return FALSE;
                }
                /* New file selected; loop back and prompt for its password */
                continue;
            }

            g_clear_error (&err);
            load_db (app_data->db_data, &err);

            if (err == NULL ||
                g_error_matches (err, missing_file_gquark (), MISSING_FILE_ERRCODE)) {
                break;
            }

            show_message_dialog (app_data->main_window, err->message, GTK_MESSAGE_ERROR);
            g_clear_pointer (&app_data->db_data->key, gcry_free);

            if (g_error_matches (err, memlock_error_gquark (), MEMLOCK_ERRCODE)) {
                g_clear_error (&err);
                return FALSE;
            }
            g_clear_error (&err);
            /* Wrong password or other recoverable error — prompt again */
        } while (TRUE);
    } else {
        load_db (app_data->db_data, &err);
        if (err != NULL &&
            !g_error_matches (err, missing_file_gquark (), MISSING_FILE_ERRCODE)) {
            show_message_dialog (app_data->main_window, err->message, GTK_MESSAGE_ERROR);
            g_clear_error (&err);
            return FALSE;
        }
    }

    if (app_data->use_secret_service && !app_data->db_data->key_stored) {
        secret_password_store (OTPCLIENT_SCHEMA, SECRET_COLLECTION_DEFAULT,
                               "main_pwd", app_data->db_data->key,
                               NULL, on_password_stored, NULL,
                               "string", "main_pwd", NULL);
    }

    if (g_error_matches (err, missing_file_gquark (), MISSING_FILE_ERRCODE)) {
        const gchar *msg = _(
            "This is the first time you run OTPClient, so you need to <b>add</b> or "
            "<b>import</b> some tokens.\n"
            "- to <b>add</b> tokens, please click the + button on the <b>top left</b>.\n"
            "- to <b>import</b> existing tokens, please click the menu button "
            "<b>on the top right</b>.\n"
            "\nIf you need more info, please visit the "
            "<a href=\"https://github.com/paolostivanin/OTPClient/wiki\">project's wiki</a>");
        show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_INFO);
        GError *tmp_err = NULL;
        update_db (app_data->db_data, &tmp_err);
        reload_db (app_data->db_data, &tmp_err);
        g_clear_error (&tmp_err);
    }

    g_clear_error (&err);
    return TRUE;
}

/* ── setup_ui_and_timers ───────────────────────────────────────────────── */

static void
setup_ui_and_timers (GApplication *app,
                     AppData      *app_data)
{
    app_data->clipboard = gtk_clipboard_get (GDK_SELECTION_CLIPBOARD);

    create_treeview (app_data);
    setup_kb_shortcuts (app_data);

    app_data->notification = g_notification_new ("OTPClient");
    g_notification_set_priority (app_data->notification, G_NOTIFICATION_PRIORITY_NORMAL);
    GIcon *icon = g_themed_icon_new ("com.github.paolostivanin.OTPClient");
    g_notification_set_icon (app_data->notification, icon);
    g_notification_set_body (app_data->notification,
                              _("OTP value has been copied to the clipboard"));
    g_object_unref (icon);

    app_data->source_id =
        g_timeout_add_full (G_PRIORITY_DEFAULT, 1000, traverse_liststore, app_data, NULL);

    setup_dbus_listener (app_data);

    app_data->last_user_activity = g_date_time_new_now_local ();
    app_data->source_id_last_activity =
        g_timeout_add_seconds (1, check_inactivity, app_data);

    (void) app;
}

/* ── finalize / class / init ───────────────────────────────────────────── */

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
    GObjectClass     *object_class = G_OBJECT_CLASS (klass);
    GApplicationClass *app_class   = G_APPLICATION_CLASS (klass);

    object_class->finalize = otpclient_application_finalize;
    app_class->activate    = otpclient_application_activate;
    app_class->shutdown    = otpclient_application_shutdown;
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

/* ── Private helpers ───────────────────────────────────────────────────── */

static void
set_config_data (gint    *width,
                 gint    *height,
                 AppData *app_data)
{
    GKeyFile *kf = get_kf_ptr ();
    if (kf != NULL) {
        *width  = g_key_file_get_integer (kf, "config", "window_width",  NULL);
        *height = g_key_file_get_integer (kf, "config", "window_height", NULL);
        app_data->show_next_otp         = g_key_file_get_boolean (kf, "config", "show_next_otp",      NULL);
        app_data->disable_notifications = g_key_file_get_boolean (kf, "config", "notifications",      NULL);
        app_data->show_validity_seconds = g_key_file_get_boolean (kf, "config", "show_validity_seconds", NULL);
        app_data->auto_lock             = g_key_file_get_boolean (kf, "config", "auto_lock",           NULL);
        app_data->inactivity_timeout    = g_key_file_get_integer (kf, "config", "inactivity_timeout",  NULL);
        app_data->use_dark_theme        = g_key_file_get_boolean (kf, "config", "dark_theme",          NULL);
        app_data->use_tray              = g_key_file_get_boolean (kf, "config", "use_tray",            NULL);
        app_data->use_secret_service    = g_key_file_get_boolean (kf, "config", "use_secret_service",  NULL);
        if (g_key_file_has_key (kf, "config", "search_provider_enabled", NULL)) {
            app_data->search_provider_enabled =
                g_key_file_get_boolean (kf, "config", "search_provider_enabled", NULL);
        } else {
            app_data->search_provider_enabled = TRUE;
        }
        load_validity_colors (kf, app_data);
        g_object_set (gtk_settings_get_default (),
                      "gtk-application-prefer-dark-theme", app_data->use_dark_theme, NULL);
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
            g_free (cfg_file_path);
            return NULL;
        }
        db_path = g_key_file_get_string (kf, "config", "db_path", NULL);
        if (db_path != NULL) {
            if (!g_file_test (db_path, G_FILE_TEST_EXISTS)) {
                gchar *msg = g_strconcat ("Database file/location:\n<b>", db_path,
                                          "</b>\ndoes not exist. A new database will be created.",
                                          NULL);
                show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
                g_free (msg);
                g_free (db_path);
                db_path = NULL;
            } else {
                g_free (cfg_file_path);
                g_key_file_free (kf);
                return db_path;
            }
        }
    }

    GtkFileChooserNative *dialog =
        gtk_file_chooser_native_new (_("Select database location"),
                                     GTK_WINDOW (app_data->main_window),
                                     app_data->open_db_file_action,
                                     "OK", "Cancel");
    GtkFileChooser *chooser = GTK_FILE_CHOOSER (dialog);
    gtk_file_chooser_set_do_overwrite_confirmation (chooser, TRUE);
    gtk_file_chooser_set_select_multiple (chooser, FALSE);
    if (app_data->open_db_file_action == GTK_FILE_CHOOSER_ACTION_SAVE) {
        gtk_file_chooser_set_current_name (chooser, "NewDatabase.enc");
    }

    if (gtk_native_dialog_run (GTK_NATIVE_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT) {
        db_path = gtk_file_chooser_get_filename (chooser);
        g_key_file_set_string (kf, "config", "db_path", db_path);
        if (!g_key_file_save_to_file (kf, cfg_file_path, &err)) {
            g_printerr ("%s\n", err->message);
            g_clear_error (&err);
        }
    }

    /* Clear any stored password so it is not used with the newly selected DB */
    secret_password_clear (OTPCLIENT_SCHEMA, NULL, on_password_cleared, NULL,
                           "string", "main_pwd", NULL);

    g_object_unref (dialog);
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
    app_data->open_db_file_action =
        g_strcmp0 (gtk_widget_get_name (btn), "diag_rc_restoredb_btn") == 0
        ? GTK_FILE_CHOOSER_ACTION_OPEN
        : GTK_FILE_CHOOSER_ACTION_SAVE;
    gtk_dialog_response (GTK_DIALOG (app_data->diag_rcdb), GTK_RESPONSE_OK);
}

static void
init_app_defaults (AppData *app_data)
{
    app_data->app_locked             = FALSE;
    app_data->show_next_otp          = FALSE;
    app_data->disable_notifications  = FALSE;
    app_data->show_validity_seconds  = FALSE;
    gdk_rgba_parse (&app_data->validity_color,         "#33A659");
    gdk_rgba_parse (&app_data->validity_warning_color, "#D95940");
    app_data->auto_lock              = FALSE;
    app_data->inactivity_timeout     = 0;
    app_data->use_dark_theme         = FALSE;
    app_data->use_secret_service     = TRUE;
    app_data->is_reorder_active      = FALSE;
    app_data->use_tray               = FALSE;
    app_data->search_provider_enabled = TRUE;
    app_data->open_db_file_action    = GTK_FILE_CHOOSER_ACTION_SAVE;
    app_data->window_width           = 0;
    app_data->window_height          = 0;
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
    app_data->db_data->key_stored                = FALSE;
    app_data->db_data->max_file_size_from_memlock = 0;
    app_data->db_data->objects_hash              = NULL;
    app_data->db_data->data_to_add               = NULL;
}

static void
cleanup_app_data (AppData *app_data)
{
    if (app_data == NULL) {
        return;
    }

    if (app_data->main_window != NULL) {
        /* Destroying the window also triggers OtpclientWindow::dispose,
         * which releases the three GtkBuilder instances. */
        gtk_widget_destroy (app_data->main_window);
    }

    /* Builders are owned by the window; do NOT unref them here. */

    if (app_data->db_data != NULL) {
        g_clear_pointer (&app_data->db_data->db_path, g_free);
        g_clear_pointer (&app_data->db_data->key, gcry_free);
        g_clear_pointer (&app_data->db_data->last_hotp, g_free);
        g_clear_pointer (&app_data->db_data->last_hotp_update, g_date_time_unref);
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
        /* destroy_cb frees app_data; clear the pointer so finalize
         * does not attempt to free it again via cleanup_app_data. */
        self->app_data = NULL;
    }

    G_APPLICATION_CLASS (otpclient_application_parent_class)->shutdown (app);
}
