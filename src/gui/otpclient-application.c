#include <glib/gi18n.h>
#include <adwaita.h>
#include "otpclient-application.h"
#include "otpclient-window.h"
#include "otp-entry.h"
#include "database-sidebar.h"
#include "gui-misc.h"
#include "dialogs/password-dialog.h"
#include "lock-app.h"
#include "dialogs/db-info-dialog.h"
#include "dialogs/kdf-dialog.h"
#include "dialogs/whats-new-dialog.h"
#include "common.h"
#include "db-common.h"
#include "gquarks.h"
#include "secret-schema.h"
#include "version.h"
#ifdef ENABLE_MINIMIZE_TO_TRAY
#include "tray.h"
#endif

struct _OTPClientApplication
{
    AdwApplication application;
    OTPClientWindow *window;

    DatabaseData *db_data;

    GCancellable *cancellable;
    GSettings *settings;

    /* config stuff */
    gboolean show_next_otp;
    gboolean disable_notifications;
    gboolean auto_lock;
    gint inactivity_timeout;
    gboolean app_locked;
    gboolean use_dark_theme;
gboolean use_secret_service;
    gboolean search_provider_enabled;
    gboolean show_validity_seconds;
    gchar *validity_color;
    gchar *validity_warning_color;
    gboolean minimize_to_tray;
    guint clipboard_clear_timeout;
};

G_DEFINE_TYPE (OTPClientApplication, otpclient_application, ADW_TYPE_APPLICATION)

static void otpclient_application_show_about      (GSimpleAction *simple,
                                                   GVariant      *parameter,
                                                   gpointer       user_data);

static void otpclient_application_quit            (GSimpleAction *simple,
                                                   GVariant      *parameter,
                                                   gpointer       user_data);

static void otpclient_application_lock            (GSimpleAction *simple,
                                                   GVariant      *parameter,
                                                   gpointer       user_data);

static void otpclient_application_shortcuts      (GSimpleAction *simple,
                                                   GVariant      *parameter,
                                                   gpointer       user_data);

static void otpclient_application_change_pwd     (GSimpleAction *simple,
                                                   GVariant      *parameter,
                                                   gpointer       user_data);

static void otpclient_application_db_info        (GSimpleAction *simple,
                                                   GVariant      *parameter,
                                                   gpointer       user_data);

static void otpclient_application_kdf_settings   (GSimpleAction *simple,
                                                   GVariant      *parameter,
                                                   gpointer       user_data);

static void otpclient_application_whats_new      (GSimpleAction *simple,
                                                   GVariant      *parameter,
                                                   gpointer       user_data);

static const GActionEntry otpclient_application_entries[] = {
        { .name = "about", .activate = otpclient_application_show_about },
        { .name = "quit", .activate = otpclient_application_quit },
        { .name = "lock", .activate = otpclient_application_lock },
        { .name = "shortcuts", .activate = otpclient_application_shortcuts },
        { .name = "change-password", .activate = otpclient_application_change_pwd },
        { .name = "db-info", .activate = otpclient_application_db_info },
        { .name = "kdf-settings", .activate = otpclient_application_kdf_settings },
        { .name = "whats-new", .activate = otpclient_application_whats_new },
};

OTPClientApplication *
otpclient_application_new (void)
{
    return g_object_new (OTPCLIENT_TYPE_APPLICATION,
                         "application-id", "com.github.paolostivanin.OTPClient",
                         "flags", G_APPLICATION_DEFAULT_FLAGS,
                         "resource-base-path", "/com/github/paolostivanin/OTPClient",
                         NULL);
}

static void
set_accel_for_action (OTPClientApplication  *self,
                      const gchar           *detailed_action_name,
                      const gchar           *accel)
{
    const char *accels[] = { accel, NULL };
    gtk_application_set_accels_for_action (GTK_APPLICATION(self), detailed_action_name, accels);
}

static void
otpclient_application_show_about (GSimpleAction *simple,
                                  GVariant      *parameter,
                                  gpointer       user_data)
{
    (void) simple;
    (void) parameter;

    static const gchar *developers[] = {
            "Paolo Stivanin <info@paolostivanin.com>",
            NULL
    };

    static const gchar *designers[] = {
            "Tobias Bernard (bertob) https://tobiasbernard.com",
            NULL
    };

    OTPClientApplication *self = OTPCLIENT_APPLICATION(user_data);

    AdwDialog *dialog = adw_about_dialog_new ();
    g_object_set (dialog,
                  "application-name", "OTPClient",
                  "application-icon", APPLICATION_ID,
                  "version", PROJECT_VER,
                  "copyright", "Copyright \xC2\xA9 2023 Paolo Stivanin",
                  "issue-url", "https://github.com/paolostivanin/OTPClient/issues",
                  "support-url", "https://github.com/paolostivanin/OTPClient/issues",
                  "website", "https://github.com/paolostivanin/OTPClient",
                  "license-type", GTK_LICENSE_GPL_3_0,
                  "developers", developers,
                  "designers", designers,
                  NULL);
    adw_dialog_present (dialog, GTK_WIDGET (self->window));
}

static void
otpclient_application_quit (GSimpleAction *simple,
                            GVariant      *parameter,
                            gpointer       user_data)
{
    (void) simple;
    (void) parameter;

    OTPClientApplication *self = OTPCLIENT_APPLICATION(user_data);
    if (self->window != NULL)
    {
        gtk_window_destroy (GTK_WINDOW(self->window));
        self->window = NULL;
    }
}

static void
otpclient_application_lock (GSimpleAction *simple,
                            GVariant      *parameter,
                            gpointer       user_data)
{
    (void) simple;
    (void) parameter;

    OTPClientApplication *self = OTPCLIENT_APPLICATION (user_data);
    lock_app_lock (self);
}

static void
otpclient_application_shortcuts (GSimpleAction *simple,
                                  GVariant      *parameter,
                                  gpointer       user_data)
{
    (void) simple;
    (void) parameter;

    OTPClientApplication *self = OTPCLIENT_APPLICATION (user_data);
    GtkBuilder *builder = gtk_builder_new_from_resource ("/com/github/paolostivanin/OTPClient/ui/shortcuts-window.ui");
    GtkWindow *dialog = GTK_WINDOW (gtk_builder_get_object (builder, "shortcuts_dialog"));
    gtk_window_set_transient_for (dialog, GTK_WINDOW (self->window));
    gtk_window_present (dialog);
    g_object_unref (builder);
}

static void
on_change_password_received (const gchar *password,
                              gpointer     user_data)
{
    OTPClientApplication *self = OTPCLIENT_APPLICATION (user_data);

    if (password == NULL || self->db_data == NULL)
        return;

    /* Update the key */
    if (self->db_data->key != NULL)
        gcry_free (self->db_data->key);

    self->db_data->key = gcry_calloc_secure (strlen (password) + 1, 1);
    memcpy (self->db_data->key, password, strlen (password) + 1);

    /* Re-encrypt the database with the new key */
    GError *err = NULL;
    update_db (self->db_data, &err);
    if (err != NULL)
    {
        g_warning ("Failed to update database with new password: %s", err->message);
        g_clear_error (&err);
        return;
    }

    /* Update secret service */
    if (self->use_secret_service)
    {
        secret_password_store (OTPCLIENT_SCHEMA,
                               SECRET_COLLECTION_DEFAULT,
                               "OTPClient database password",
                               self->db_data->key,
                               NULL,
                               on_password_stored,
                               NULL,
                               "string", self->db_data->db_path,
                               NULL);
    }
}

static void
otpclient_application_change_pwd (GSimpleAction *simple,
                                   GVariant      *parameter,
                                   gpointer       user_data)
{
    (void) simple;
    (void) parameter;

    OTPClientApplication *self = OTPCLIENT_APPLICATION (user_data);
    if (self->db_data == NULL)
        return;

    PasswordDialog *dlg = password_dialog_new (PASSWORD_MODE_CHANGE,
                                               on_change_password_received,
                                               self);
    adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self->window));
}

static void
otpclient_application_db_info (GSimpleAction *simple,
                                GVariant      *parameter,
                                gpointer       user_data)
{
    (void) simple;
    (void) parameter;

    OTPClientApplication *self = OTPCLIENT_APPLICATION (user_data);
    if (self->db_data == NULL)
        return;

    DbInfoDialog *dlg = db_info_dialog_new (self->db_data);
    adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self->window));
}

static void
otpclient_application_kdf_settings (GSimpleAction *simple,
                                     GVariant      *parameter,
                                     gpointer       user_data)
{
    (void) simple;
    (void) parameter;

    OTPClientApplication *self = OTPCLIENT_APPLICATION (user_data);
    if (self->db_data == NULL)
        return;

    KdfDialog *dlg = kdf_dialog_new (self->db_data);
    adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self->window));
}

static gboolean
version_xy_less_than (const gchar *a,
                      const gchar *b)
{
    gint ax = 0, ay = 0, bx = 0, by = 0;
    sscanf (a, "%d.%d", &ax, &ay);
    sscanf (b, "%d.%d", &bx, &by);
    return (ax < bx) || (ax == bx && ay < by);
}

static void
otpclient_application_whats_new (GSimpleAction *simple,
                                  GVariant      *parameter,
                                  gpointer       user_data)
{
    (void) simple;
    (void) parameter;

    OTPClientApplication *self = OTPCLIENT_APPLICATION (user_data);
    WhatsNewDialog *dlg = whats_new_dialog_new (FALSE);
    adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self->window));
}

static void
populate_window_from_db (OTPClientApplication *self)
{
    if (self->db_data == NULL || self->db_data->in_memory_json_data == NULL)
        return;

    GListStore *store = otpclient_window_get_otp_store (self->window);
    if (store == NULL)
        return;

    g_list_store_remove_all (store);

    json_t *json_db = self->db_data->in_memory_json_data;
    gsize index;
    json_t *obj;

    json_array_foreach (json_db, index, obj)
    {
        const gchar *type = json_string_value (json_object_get (obj, "type"));
        const gchar *label = json_string_value (json_object_get (obj, "label"));
        const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
        const gchar *secret = json_string_value (json_object_get (obj, "secret"));
        const gchar *algo = json_string_value (json_object_get (obj, "algo"));
        guint32 digits = (guint32) json_integer_value (json_object_get (obj, "digits"));
        guint32 period = 30;
        guint64 counter = 0;

        if (digits < 4) digits = 6;

        if (type != NULL && g_ascii_strcasecmp (type, "HOTP") == 0)
            counter = (guint64) json_integer_value (json_object_get (obj, "counter"));
        else
            period = (guint32) json_integer_value (json_object_get (obj, "period"));

        if (period < 1) period = 30;

        OTPEntry *entry = otp_entry_new (label, issuer, NULL,
                                          type ? type : "TOTP",
                                          period, counter,
                                          algo ? algo : "SHA1",
                                          digits, secret);
        const gchar *group = json_string_value (json_object_get (obj, "group"));
        if (group != NULL)
            otp_entry_set_group (entry, group);

        otp_entry_update_otp (entry);

        g_list_store_append (store, entry);
        g_object_unref (entry);
    }
}

static void
otpclient_application_activate (GApplication *application)
{
    OTPClientApplication *self = OTPCLIENT_APPLICATION(application);
    gtk_window_present (GTK_WINDOW(self->window));
}

static void
on_password_received (const gchar *password,
                      gpointer     user_data)
{
    OTPClientApplication *self = OTPCLIENT_APPLICATION (user_data);

    if (password == NULL || self->db_data == NULL)
        return;

    self->db_data->key = gcry_calloc_secure (strlen (password) + 1, 1);
    memcpy (self->db_data->key, password, strlen (password) + 1);

    GError *err = NULL;
    load_db (self->db_data, &err);
    if (err != NULL)
    {
        g_warning ("Failed to load database: %s", err->message);

        /* Show password dialog again on wrong password */
        if (err->code == BAD_TAG_ERRCODE)
        {
            gcry_free (self->db_data->key);
            self->db_data->key = NULL;
            g_clear_error (&err);

            if (self->window != NULL)
            {
                PasswordDialog *dlg = password_dialog_new (PASSWORD_MODE_DECRYPT,
                                                           on_password_received,
                                                           self);
                adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self->window));
            }
            return;
        }
        g_clear_error (&err);
        return;
    }

    /* Store password in secret service if enabled */
    if (self->use_secret_service)
    {
        secret_password_store (OTPCLIENT_SCHEMA,
                               SECRET_COLLECTION_DEFAULT,
                               "OTPClient database password",
                               self->db_data->key,
                               NULL,
                               on_password_stored,
                               NULL,
                               "string", self->db_data->db_path,
                               NULL);
    }

    populate_window_from_db (self);
    if (self->window != NULL)
        otpclient_window_start_otp_timer (self->window);
}

static void
on_secret_lookup_done (GObject      *source,
                       GAsyncResult *result,
                       gpointer      user_data)
{
    (void) source;
    OTPClientApplication *self = OTPCLIENT_APPLICATION (user_data);

    GError *err = NULL;
    gchar *password = secret_password_lookup_finish (result, &err);

    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
        g_clear_error (&err);
        return;
    }

    if (password != NULL)
    {
        on_password_received (password, self);
        secret_password_free (password);
    }
    else if (self->window != NULL)
    {
        /* No stored password — show the password dialog */
        PasswordDialog *dlg = password_dialog_new (PASSWORD_MODE_DECRYPT,
                                                   on_password_received,
                                                   self);
        adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self->window));
    }

    g_clear_error (&err);
}

static void
init_database (OTPClientApplication *self)
{
    gint32 memlock_value = 0;
    gint32 memlock_status = set_memlock_value (&memlock_value);

    if (memlock_status == MEMLOCK_ERR)
    {
        g_warning ("Could not retrieve memlock value");
        memlock_value = DEFAULT_MEMLOCK_VALUE;
    }

    gchar *init_err = init_libs (memlock_value);
    if (init_err != NULL)
    {
        g_warning ("Failed to initialize crypto libraries: %s", init_err);
        g_free (init_err);
        return;
    }

    /* Load the full database list (handles v4 migration) */
    g_autoptr (GPtrArray) db_list = gui_misc_get_db_list ();
    if (db_list == NULL || db_list->len == 0)
    {
        g_info ("No databases configured yet");
        return;
    }

    /* Populate sidebar with all known databases */
    for (guint i = 0; i < db_list->len; i++)
    {
        DatabaseEntry *entry = g_ptr_array_index (db_list, i);
        otpclient_window_add_database (self->window,
                                       database_entry_get_name (entry),
                                       database_entry_get_path (entry));
    }

    /* Find and select the primary database */
    g_autofree gchar *primary_path = gui_misc_get_db_path_from_cfg ();
    gint primary_index = 0;
    if (primary_path != NULL) {
        for (guint i = 0; i < db_list->len; i++) {
            DatabaseEntry *entry = g_ptr_array_index (db_list, i);
            if (g_strcmp0 (database_entry_get_path (entry), primary_path) == 0) {
                primary_index = (gint)i;
                break;
            }
        }
    }
    otpclient_window_select_database (self->window, primary_index);

    /* Set up the primary database for decryption */
    DatabaseEntry *primary_entry = g_ptr_array_index (db_list, primary_index);
    const gchar *db_path = database_entry_get_path (primary_entry);

    self->db_data = g_new0 (DatabaseData, 1);
    self->db_data->db_path = g_strdup (db_path);
    self->db_data->max_file_size_from_memlock = memlock_value;

    if (self->use_secret_service)
    {
        secret_password_lookup (OTPCLIENT_SCHEMA, self->cancellable,
                                on_secret_lookup_done, self,
                                "string", self->db_data->db_path,
                                NULL);
    }
    else
    {
        PasswordDialog *dlg = password_dialog_new (PASSWORD_MODE_DECRYPT,
                                                   on_password_received,
                                                   self);
        adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self->window));
    }
}

static void
otpclient_application_startup (GApplication *application)
{
    OTPClientApplication *self = OTPCLIENT_APPLICATION(application);

    g_action_map_add_action_entries (G_ACTION_MAP(self),
                                     otpclient_application_entries,
                                     G_N_ELEMENTS(otpclient_application_entries),
                                     self);

    set_accel_for_action (self, "app.about", "<Control>b");
    set_accel_for_action (self, "app.lock", "<Control>l");
    set_accel_for_action (self, "app.shortcuts", "<Control>question");

    /* Load settings from GSettings if schema is available */
    self->app_locked = FALSE;
GSettingsSchemaSource *schema_source = g_settings_schema_source_get_default ();
    g_autoptr (GSettingsSchema) schema = NULL;
    if (schema_source != NULL)
        schema = g_settings_schema_source_lookup (schema_source,
                    "com.github.paolostivanin.OTPClient", TRUE);

    if (schema != NULL) {
        self->settings = g_settings_new ("com.github.paolostivanin.OTPClient");
        self->show_next_otp = g_settings_get_boolean (self->settings, "show-next-otp");
        self->disable_notifications = !g_settings_get_boolean (self->settings, "notification-enabled");
        self->auto_lock = g_settings_get_boolean (self->settings, "auto-lock");
        self->inactivity_timeout = (gint)g_settings_get_uint (self->settings, "auto-lock-timeout");
        self->use_dark_theme = g_settings_get_boolean (self->settings, "dark-theme");
        self->use_secret_service = g_settings_get_boolean (self->settings, "secret-service");
        self->search_provider_enabled = g_settings_get_boolean (self->settings, "search-provider-enabled");
        self->show_validity_seconds = g_settings_get_boolean (self->settings, "show-validity-seconds");
        self->validity_color = g_settings_get_string (self->settings, "validity-color");
        self->validity_warning_color = g_settings_get_string (self->settings, "validity-warning-color");
        self->minimize_to_tray = g_settings_get_boolean (self->settings, "minimize-to-tray");
        self->clipboard_clear_timeout = g_settings_get_uint (self->settings, "clipboard-clear-timeout");
    } else {
        self->settings = NULL;
        self->show_next_otp = FALSE;
        self->disable_notifications = FALSE;
        self->auto_lock = FALSE;
        self->inactivity_timeout = 0;
        self->use_dark_theme = FALSE;
        self->use_secret_service = FALSE;
        self->search_provider_enabled = TRUE;
        self->show_validity_seconds = FALSE;
        self->validity_color = g_strdup ("#008000");
        self->validity_warning_color = g_strdup ("#ffa500");
        self->minimize_to_tray = FALSE;
        self->clipboard_clear_timeout = 30;
    }

    /* Apply dark theme preference */
    if (self->use_dark_theme)
        adw_style_manager_set_color_scheme (adw_style_manager_get_default (),
                                            ADW_COLOR_SCHEME_FORCE_DARK);

    G_APPLICATION_CLASS (otpclient_application_parent_class)->startup (application);

    gtk_window_set_default_icon_name (APPLICATION_ID);
    self->window = OTPCLIENT_WINDOW(otpclient_window_new (self));

    lock_app_init_dbus_watchers (self);

#ifdef ENABLE_MINIMIZE_TO_TRAY
    otpclient_tray_init (self);
#endif

    /* Show welcome or what's-new dialog on version change */
    if (self->settings != NULL) {
        g_autofree gchar *last_seen = g_settings_get_string (self->settings, "last-seen-version");
        if (last_seen == NULL || last_seen[0] == '\0') {
            WhatsNewDialog *dlg = whats_new_dialog_new (TRUE);
            adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self->window));
        } else if (version_xy_less_than (last_seen, PROJECT_VER)) {
            WhatsNewDialog *dlg = whats_new_dialog_new (FALSE);
            adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self->window));
        }
        g_settings_set_string (self->settings, "last-seen-version", PROJECT_VER);
    }

    init_database (self);
}

static void
otpclient_application_dispose (GObject *object)
{
    OTPClientApplication *self = OTPCLIENT_APPLICATION (object);

    g_cancellable_cancel (self->cancellable);
    g_clear_object (&self->cancellable);

#ifdef ENABLE_MINIMIZE_TO_TRAY
    otpclient_tray_cleanup (self);
#endif

    lock_app_cleanup (self);

    self->window = NULL;

    g_clear_object (&self->settings);
    g_clear_pointer (&self->validity_color, g_free);
    g_clear_pointer (&self->validity_warning_color, g_free);

    if (self->db_data != NULL)
    {
        if (self->db_data->in_memory_json_data != NULL)
            json_decref (self->db_data->in_memory_json_data);
        if (self->db_data->key != NULL)
            gcry_free (self->db_data->key);
        g_free (self->db_data->db_path);
        g_slist_free_full (self->db_data->objects_hash, g_free);
        g_free (self->db_data->last_hotp);
        if (self->db_data->last_hotp_update != NULL)
            g_date_time_unref (self->db_data->last_hotp_update);
        g_free (self->db_data);
        self->db_data = NULL;
    }

    G_OBJECT_CLASS (otpclient_application_parent_class)->dispose (object);
}

static void
otpclient_application_class_init (OTPClientApplicationClass *klass)
{
    GApplicationClass *application_class = G_APPLICATION_CLASS(klass);
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    application_class->activate = otpclient_application_activate;
    application_class->startup = otpclient_application_startup;
    object_class->dispose = otpclient_application_dispose;
}

static void
otpclient_application_init (OTPClientApplication *self)
{
    self->db_data = NULL;
    self->cancellable = g_cancellable_new ();
    self->settings = NULL;
    self->validity_color = NULL;
    self->validity_warning_color = NULL;
}

DatabaseData *
otpclient_application_get_db_data (OTPClientApplication *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_APPLICATION (self), NULL);
    return self->db_data;
}

void
otpclient_application_set_db_data (OTPClientApplication *self,
                                    DatabaseData         *db_data)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (self));

    if (self->db_data != NULL)
    {
        if (self->db_data->in_memory_json_data != NULL)
            json_decref (self->db_data->in_memory_json_data);
        if (self->db_data->key != NULL)
            gcry_free (self->db_data->key);
        g_free (self->db_data->db_path);
        g_slist_free_full (self->db_data->objects_hash, g_free);
        g_free (self->db_data->last_hotp);
        if (self->db_data->last_hotp_update != NULL)
            g_date_time_unref (self->db_data->last_hotp_update);
        g_free (self->db_data);
    }

    self->db_data = db_data;
}

gboolean otpclient_application_get_show_next_otp (OTPClientApplication *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_APPLICATION (self), FALSE);
    return self->show_next_otp;
}

void otpclient_application_set_show_next_otp (OTPClientApplication *self, gboolean show)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (self));
    self->show_next_otp = show;
    if (self->settings != NULL)
        g_settings_set_boolean (self->settings, "show-next-otp", show);
}

gboolean otpclient_application_get_disable_notifications (OTPClientApplication *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_APPLICATION (self), FALSE);
    return self->disable_notifications;
}

void otpclient_application_set_disable_notifications (OTPClientApplication *self, gboolean disable)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (self));
    self->disable_notifications = disable;
    if (self->settings != NULL)
        g_settings_set_boolean (self->settings, "notification-enabled", !disable);
}

gboolean otpclient_application_get_auto_lock (OTPClientApplication *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_APPLICATION (self), FALSE);
    return self->auto_lock;
}

void otpclient_application_set_auto_lock (OTPClientApplication *self, gboolean auto_lock)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (self));
    self->auto_lock = auto_lock;
    if (self->settings != NULL)
        g_settings_set_boolean (self->settings, "auto-lock", auto_lock);
}

gint otpclient_application_get_inactivity_timeout (OTPClientApplication *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_APPLICATION (self), 0);
    return self->inactivity_timeout;
}

void otpclient_application_set_inactivity_timeout (OTPClientApplication *self, gint timeout)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (self));
    self->inactivity_timeout = timeout;
    if (self->settings != NULL)
        g_settings_set_uint (self->settings, "auto-lock-timeout", (guint)timeout);
}

gboolean otpclient_application_get_app_locked (OTPClientApplication *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_APPLICATION (self), FALSE);
    return self->app_locked;
}

void otpclient_application_set_app_locked (OTPClientApplication *self, gboolean locked)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (self));
    self->app_locked = locked;
}

gboolean otpclient_application_get_use_dark_theme (OTPClientApplication *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_APPLICATION (self), FALSE);
    return self->use_dark_theme;
}

void otpclient_application_set_use_dark_theme (OTPClientApplication *self, gboolean use_dark)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (self));
    self->use_dark_theme = use_dark;
    if (self->settings != NULL)
        g_settings_set_boolean (self->settings, "dark-theme", use_dark);
}

gboolean otpclient_application_get_use_secret_service (OTPClientApplication *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_APPLICATION (self), FALSE);
    return self->use_secret_service;
}

void otpclient_application_set_use_secret_service (OTPClientApplication *self, gboolean use_ss)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (self));
    self->use_secret_service = use_ss;
    if (self->settings != NULL)
        g_settings_set_boolean (self->settings, "secret-service", use_ss);
}

gboolean otpclient_application_get_search_provider_enabled (OTPClientApplication *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_APPLICATION (self), TRUE);
    return self->search_provider_enabled;
}

void otpclient_application_set_search_provider_enabled (OTPClientApplication *self, gboolean enabled)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (self));
    self->search_provider_enabled = enabled;
    if (self->settings != NULL)
        g_settings_set_boolean (self->settings, "search-provider-enabled", enabled);
}

gboolean otpclient_application_get_show_validity_seconds (OTPClientApplication *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_APPLICATION (self), FALSE);
    return self->show_validity_seconds;
}

void otpclient_application_set_show_validity_seconds (OTPClientApplication *self, gboolean show)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (self));
    self->show_validity_seconds = show;
    if (self->settings != NULL)
        g_settings_set_boolean (self->settings, "show-validity-seconds", show);
}

const gchar *otpclient_application_get_validity_color (OTPClientApplication *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_APPLICATION (self), "green");
    return self->validity_color;
}

void otpclient_application_set_validity_color (OTPClientApplication *self, const gchar *color)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (self));
    g_free (self->validity_color);
    self->validity_color = g_strdup (color);
    if (self->settings != NULL)
        g_settings_set_string (self->settings, "validity-color", color);
}

const gchar *otpclient_application_get_validity_warning_color (OTPClientApplication *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_APPLICATION (self), "orange");
    return self->validity_warning_color;
}

void otpclient_application_set_validity_warning_color (OTPClientApplication *self, const gchar *color)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (self));
    g_free (self->validity_warning_color);
    self->validity_warning_color = g_strdup (color);
    if (self->settings != NULL)
        g_settings_set_string (self->settings, "validity-warning-color", color);
}

gboolean otpclient_application_get_minimize_to_tray (OTPClientApplication *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_APPLICATION (self), FALSE);
    return self->minimize_to_tray;
}

void otpclient_application_set_minimize_to_tray (OTPClientApplication *self, gboolean minimize)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (self));
    self->minimize_to_tray = minimize;
    if (self->settings != NULL)
        g_settings_set_boolean (self->settings, "minimize-to-tray", minimize);

#ifdef ENABLE_MINIMIZE_TO_TRAY
    if (minimize)
        otpclient_tray_enable (self);
    else
        otpclient_tray_disable (self);
#endif
}

void otpclient_application_reload_settings (OTPClientApplication *self)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (self));
    if (self->settings == NULL)
        return;

    self->show_next_otp = g_settings_get_boolean (self->settings, "show-next-otp");
    self->disable_notifications = !g_settings_get_boolean (self->settings, "notification-enabled");
    self->auto_lock = g_settings_get_boolean (self->settings, "auto-lock");
    self->inactivity_timeout = (gint) g_settings_get_uint (self->settings, "auto-lock-timeout");
    self->use_dark_theme = g_settings_get_boolean (self->settings, "dark-theme");
    self->use_secret_service = g_settings_get_boolean (self->settings, "secret-service");
    self->search_provider_enabled = g_settings_get_boolean (self->settings, "search-provider-enabled");
    self->show_validity_seconds = g_settings_get_boolean (self->settings, "show-validity-seconds");
    g_free (self->validity_color);
    self->validity_color = g_settings_get_string (self->settings, "validity-color");
    g_free (self->validity_warning_color);
    self->validity_warning_color = g_settings_get_string (self->settings, "validity-warning-color");
    self->minimize_to_tray = g_settings_get_boolean (self->settings, "minimize-to-tray");
    self->clipboard_clear_timeout = g_settings_get_uint (self->settings, "clipboard-clear-timeout");

    /* Apply dark theme */
    adw_style_manager_set_color_scheme (adw_style_manager_get_default (),
                                        self->use_dark_theme ? ADW_COLOR_SCHEME_FORCE_DARK
                                                             : ADW_COLOR_SCHEME_DEFAULT);
}

guint otpclient_application_get_clipboard_clear_timeout (OTPClientApplication *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_APPLICATION (self), 30);
    return self->clipboard_clear_timeout;
}

void otpclient_application_set_clipboard_clear_timeout (OTPClientApplication *self, guint timeout)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (self));
    self->clipboard_clear_timeout = timeout;
    if (self->settings != NULL)
        g_settings_set_uint (self->settings, "clipboard-clear-timeout", timeout);
}
