#define _DEFAULT_SOURCE
#include <glib/gi18n.h>
#include <glib-unix.h>
#include <adwaita.h>
#include <string.h>
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
    gchar *search_provider_keyword;
    gboolean show_validity_seconds;
    gchar *validity_color;
    gchar *validity_warning_color;
    gboolean minimize_to_tray;
    guint clipboard_clear_timeout;
    gboolean hide_otps;

    /* Set TRUE while the password being used to unlock came from the v4
     * legacy keyring entry (issue #448). on_unlock_done consults it to
     * decide whether to clear the legacy entry after a successful unlock;
     * reset on every unlock outcome so a bad-password retry can't destroy
     * the only copy of the password. */
    gboolean migrating_legacy_keyring;

    /* TRUE between the moment on_password_received hands db_data to the
     * worker thread and on_unlock_done firing back on the main thread.
     * Window-side actions that would free db_data (sidebar switch, Open/New
     * DB) consult otpclient_application_is_unlocking and refuse during this
     * window: the worker still holds a raw pointer to db_data and freeing
     * it under the worker's feet is a use-after-free. */
    gboolean unlock_in_progress;
};

G_DEFINE_TYPE (OTPClientApplication, otpclient_application, ADW_TYPE_APPLICATION)

enum
{
    PROP_0,
    PROP_HIDE_OTPS,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

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

    static const gchar *artists[] = {
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
                  "artists", artists,
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

/* Issue #446: when a libsecret call fails at runtime with a real error
 * (i.e. not "no password stored"), the registered Secret Service provider
 * is broken in some way we can't recover from in-process — libsecret has
 * no fallback to a different keyring. Flip the setting OFF so we don't
 * loop on the failure every launch, and surface one notification so the
 * user knows what happened and how to re-enable it later. */
static void
application_disable_secret_service_runtime (OTPClientApplication *self,
                                            const gchar          *libsecret_err_msg)
{
    if (!self->use_secret_service)
        return;

    otpclient_application_set_use_secret_service (self, FALSE);

    g_autofree gchar *body = g_strdup_printf (
        _("OTPClient's system-keyring integration was disabled because the keyring "
          "rejected the request. Re-enable it in Settings once your keyring is available."
          "\n\nError: %s"),
        libsecret_err_msg != NULL ? libsecret_err_msg : _("unknown error"));

    gui_misc_send_notification (G_APPLICATION (self),
                                _("Secret Service disabled"),
                                body);
}

typedef struct {
    GWeakRef app_ref;
} ApplicationAsyncContext;

static ApplicationAsyncContext *
application_async_context_new (OTPClientApplication *self)
{
    ApplicationAsyncContext *ctx = g_new0 (ApplicationAsyncContext, 1);
    g_weak_ref_init (&ctx->app_ref, self);
    return ctx;
}

static void
application_async_context_free (ApplicationAsyncContext *ctx)
{
    if (ctx == NULL)
        return;
    g_weak_ref_clear (&ctx->app_ref);
    g_free (ctx);
}

typedef struct {
    GWeakRef app_ref;
    gboolean clear_legacy_on_success;
} StorePasswordContext;

static gboolean
password_bytes_equal (const gchar *a,
                      const gchar *b)
{
    if (a == NULL || b == NULL)
        return FALSE;

    gsize a_len = strlen (a);
    gsize b_len = strlen (b);
    volatile guchar result = (a_len != b_len);
    gsize cmp_len = (a_len < b_len) ? a_len : b_len;
    for (gsize i = 0; i < cmp_len; i++)
        result |= ((const volatile guchar *) a)[i] ^ ((const volatile guchar *) b)[i];

    return result == 0;
}

static void
on_password_stored_gui (GObject      *source         __attribute__((unused)),
                        GAsyncResult *result,
                        gpointer      user_data)
{
    StorePasswordContext *ctx = user_data;
    g_autoptr (OTPClientApplication) self = g_weak_ref_get (&ctx->app_ref);
    const gboolean clear_legacy = ctx->clear_legacy_on_success;
    g_weak_ref_clear (&ctx->app_ref);
    g_free (ctx);

    GError *err = NULL;
    secret_password_store_finish (result, &err);

    if (self == NULL) {
        g_clear_error (&err);
        return;
    }

    if (err != NULL) {
        application_disable_secret_service_runtime (self, err->message);
        g_error_free (err);
    } else if (clear_legacy) {
        otpclient_secret_clear_legacy_async ();
    }
}

static gboolean
on_change_password_received (const gchar  *current_password,
                              const gchar  *password,
                              gchar       **error_message,
                              gpointer      user_data)
{
    OTPClientApplication *self = OTPCLIENT_APPLICATION (user_data);

    if (password == NULL || self->db_data == NULL)
        return FALSE;

    if (!password_bytes_equal (current_password, self->db_data->key))
    {
        if (error_message != NULL)
            *error_message = g_strdup (_("Current password is incorrect"));
        return FALSE;
    }

    GError *err = NULL;
    db_change_password (self->db_data, password, &err);
    if (err != NULL)
    {
        g_warning ("Failed to update database with new password: %s", err->message);
        if (error_message != NULL)
            *error_message = g_strdup (err->message);
        g_clear_error (&err);
        return FALSE;
    }

    /* Update secret service */
    if (self->use_secret_service)
    {
        StorePasswordContext *ctx = g_new0 (StorePasswordContext, 1);
        g_weak_ref_init (&ctx->app_ref, self);
        secret_password_store (OTPCLIENT_SCHEMA,
                               SECRET_COLLECTION_DEFAULT,
                               "OTPClient database password",
                               self->db_data->key,
                               NULL,
                               on_password_stored_gui,
                               ctx,
                               "string", self->db_data->db_path,
                               NULL);
    }

    return TRUE;
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
    /* If either parse fails (malformed last-seen-version) treat as "no
     * upgrade" rather than firing a misleading What's New dialog. */
    if (sscanf (a, "%d.%d", &ax, &ay) != 2)
        return FALSE;
    if (sscanf (b, "%d.%d", &bx, &by) != 2)
        return FALSE;
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
    /* Window can be NULL if it was destroyed while the unlock thread was
     * still running (150–300 ms Argon2id derive). Bail rather than deref. */
    if (self->window == NULL)
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

    otpclient_window_rebuild_groups (self->window);
    otpclient_window_set_db_actions_enabled (self->window, TRUE);
}

static void
otpclient_application_activate (GApplication *application)
{
    OTPClientApplication *self = OTPCLIENT_APPLICATION(application);
    gtk_window_present (GTK_WINDOW(self->window));
}

static gboolean on_password_received (const gchar  *current_password,
                                      const gchar  *password,
                                      gchar       **error_message,
                                      gpointer      user_data);

static void
present_db_missing_dialog (OTPClientWindow *win,
                           const gchar     *db_name,
                           const gchar     *db_path)
{
    if (win == NULL)
        return;

    g_autofree gchar *body = g_strdup_printf (
        _("“%s” could not be found at:\n%s\n\n"
          "The file may have been moved or deleted. "
          "Use the sidebar to switch to a different database, "
          "or right-click the missing entry to remove it from the list."),
        db_name != NULL ? db_name : "",
        db_path != NULL ? db_path : "");

    AdwAlertDialog *dialog = ADW_ALERT_DIALOG (
        adw_alert_dialog_new (_("Database File Not Found"), body));
    adw_alert_dialog_add_response (dialog, "ok", _("OK"));
    adw_alert_dialog_set_default_response (dialog, "ok");
    adw_alert_dialog_set_close_response (dialog, "ok");

    adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (win));
}

static void
unlock_db_thread (GTask        *task,
                  gpointer      source_object,
                  gpointer      task_data,
                  GCancellable *cancellable)
{
    (void) source_object;
    (void) cancellable;
    DatabaseData *db_data = task_data;
    GError *err = NULL;
    load_db (db_data, &err);
    if (err != NULL)
        g_task_return_error (task, err);
    else
        g_task_return_boolean (task, TRUE);
}

static void
on_unlock_done (GObject      *source_object,
                GAsyncResult *result,
                gpointer      user_data)
{
    (void) source_object;
    ApplicationAsyncContext *ctx = user_data;
    g_autoptr (OTPClientApplication) self = g_weak_ref_get (&ctx->app_ref);
    application_async_context_free (ctx);

    GError *err = NULL;
    g_task_propagate_boolean (G_TASK (result), &err);

    if (self == NULL) {
        g_clear_error (&err);
        return;
    }

    /* The worker has returned and is no longer reading db_data, so it is
     * safe again for the window to swap or free it. Clear up-front so
     * every early-return path below uncovers the lock automatically. */
    self->unlock_in_progress = FALSE;
    if (err != NULL)
    {
        g_warning ("Failed to load database: %s", err->message);

        if (self->window != NULL)
            otpclient_window_hide_loading (self->window);

        /* Race: file vanished between init_database()'s validation and the
         * unlock thread reading it. Mark the sidebar entry, drop db_data so
         * update_empty_state() routes to "no-db", and surface the same
         * dialog as the startup path. */
        if (g_error_matches (err, missing_file_gquark (), MISSING_FILE_ERRCODE))
        {
            g_autofree gchar *missing_path = self->db_data != NULL
                ? g_strdup (self->db_data->db_path) : NULL;
            g_autofree gchar *missing_name = NULL;

            if (self->window != NULL && missing_path != NULL)
            {
                GListStore *store = otpclient_window_get_db_store (self->window);
                guint n = g_list_model_get_n_items (G_LIST_MODEL (store));
                for (guint i = 0; i < n; i++)
                {
                    g_autoptr (DatabaseEntry) e = g_list_model_get_item (
                        G_LIST_MODEL (store), i);
                    if (g_strcmp0 (database_entry_get_path (e), missing_path) == 0)
                    {
                        if (missing_name == NULL)
                            missing_name = g_strdup (database_entry_get_name (e));
                        database_entry_set_missing (e, TRUE);
                        break;
                    }
                }
            }

            otpclient_application_set_db_data (self, NULL);

            if (self->window != NULL)
            {
                present_db_missing_dialog (self->window, missing_name, missing_path);
                otpclient_window_hide_loading (self->window);
            }

            g_clear_error (&err);
            return;
        }

        /* Show password dialog again on wrong password */
        if (err->code == BAD_TAG_ERRCODE)
        {
            /* Belt-and-braces: drop any cached derived key so a stale entry
             * (e.g. from a prior successful unlock with a since-changed file)
             * can't be returned to the next attempt via the salt-keyed cache. */
            db_invalidate_kdf_cache (self->db_data);
            gcry_free (self->db_data->key);
            self->db_data->key = NULL;
            g_clear_error (&err);

            /* The legacy entry holds the only copy of the v4 password until
             * we successfully migrate it; never clear it on a bad-password
             * attempt. The next on_secret_lookup_done will re-discover it. */
            self->migrating_legacy_keyring = FALSE;

            if (self->window != NULL)
            {
                PasswordDialog *dlg = password_dialog_new (PASSWORD_MODE_DECRYPT,
                                                           on_password_received,
                                                           self);
                lock_app_install_unlock_dialog_quit (dlg, self);
                adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self->window));
            }
            return;
        }

        /* Any other failure (file too big, malformed header, secmem
         * exhaustion, KDF param out-of-range, ...) is a dead-end: surface it
         * to the user instead of leaving them staring at an empty window.
         * Drop db_data so update_empty_state routes to "no-db" instead of
         * the "Unlocking…" page (which keys off in_memory_json_data == NULL);
         * the sidebar entry stays put so the user can click it to retry. */
        if (self->window != NULL)
        {
            g_autofree gchar *msg = g_strdup_printf (_("Could not open database: %s"), err->message);
            otpclient_window_show_error_toast (self->window, msg);
        }
        otpclient_application_set_db_data (self, NULL);
        self->migrating_legacy_keyring = FALSE;
        g_clear_error (&err);
        return;
    }

    /* Store password in secret service if enabled */
    if (self->use_secret_service)
    {
        StorePasswordContext *ctx = g_new0 (StorePasswordContext, 1);
        g_weak_ref_init (&ctx->app_ref, self);
        ctx->clear_legacy_on_success = self->migrating_legacy_keyring;
        secret_password_store (OTPCLIENT_SCHEMA,
                               SECRET_COLLECTION_DEFAULT,
                               "OTPClient database password",
                               self->db_data->key,
                               NULL,
                               on_password_stored_gui,
                               ctx,
                               "string", self->db_data->db_path,
                               NULL);
    }

    /* Issue #448: the password we just used came from the v4 legacy keyring
     * entry. It is now (or about to be) stored under the v5 db_path key, so
     * drop the legacy entry to keep the keyring tidy and avoid re-running
     * the fallback on future launches. Async clear: failures are surfaced
     * via on_password_cleared but do not affect the unlock outcome. */
    self->migrating_legacy_keyring = FALSE;

    /* Populate first, *then* swap out the loading page, otherwise
     * non-empty databases briefly flash the empty-state placeholder. */
    populate_window_from_db (self);
    if (self->window != NULL)
    {
        otpclient_window_hide_loading (self->window);
        otpclient_window_start_otp_timer (self->window);
        otpclient_window_sync_active_flag (self->window);
    }
}

static gboolean
on_password_received (const gchar  *current_password,
                      const gchar  *password,
                      gchar       **error_message,
                      gpointer      user_data)
{
    (void) current_password;
    (void) error_message;
    OTPClientApplication *self = OTPCLIENT_APPLICATION (user_data);

    if (password == NULL || self->db_data == NULL)
        return FALSE;

    gsize pwd_len = strlen (password);
    self->db_data->key = gcry_calloc_secure (pwd_len + 1, 1);
    memcpy (self->db_data->key, password, pwd_len + 1);

    /* Defense in depth: every observed caller already wipes its own buffer
     * (password-dialog gcry_free's a secmem copy, secret_password_free wipes
     * libsecret-owned memory), but wiping here guarantees no plaintext
     * survives the memcpy even if a future caller forgets. The cast
     * discards const; password is documented as caller-owned and consumed
     * by this function. */
    if (pwd_len > 0)
        explicit_bzero ((gchar *) password, pwd_len);

    if (self->window != NULL)
        otpclient_window_show_loading (self->window);

    /* Mark the unlock as in-flight before kicking off the worker. While
     * this is TRUE, the window refuses any action that would free db_data
     * (sidebar switch, Open/New DB), because the worker still holds a
     * raw pointer to it. Cleared at the top of on_unlock_done. */
    self->unlock_in_progress = TRUE;

    GTask *task = g_task_new (self, self->cancellable, on_unlock_done,
                              application_async_context_new (self));
    g_task_set_task_data (task, self->db_data, NULL);
    g_task_run_in_thread (task, unlock_db_thread);
    g_object_unref (task);
    return TRUE;
}

static void
on_secret_lookup_done (GObject      *source,
                       GAsyncResult *result,
                       gpointer      user_data)
{
    (void) source;
    ApplicationAsyncContext *ctx = user_data;
    g_autoptr (OTPClientApplication) self = g_weak_ref_get (&ctx->app_ref);
    application_async_context_free (ctx);

    GError *err = NULL;
    gchar *password = secret_password_lookup_finish (result, &err);

    if (self == NULL)
    {
        if (password != NULL)
            secret_password_free (password);
        g_clear_error (&err);
        return;
    }

    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
    {
        if (password != NULL)
            secret_password_free (password);
        g_clear_error (&err);
        return;
    }

    if (password != NULL)
    {
        self->migrating_legacy_keyring = FALSE;
        on_password_received (NULL, password, NULL, self);
        secret_password_free (password);
        g_clear_error (&err);
        return;
    }

    /* Issue #448: v5 keys keyring entries by absolute db_path; pre-5.0 used
     * a fixed "main_pwd" attribute. If the primary lookup returned "not
     * stored" (NULL with no error), try the legacy entry before falling
     * back to the password dialog. The sync call here is acceptable: it
     * only runs on the (rare) failure path, and a successful unlock then
     * migrates the entry over via on_unlock_done. */
    if (err == NULL)
    {
        GError *legacy_err = NULL;
        gchar *legacy_pwd = otpclient_secret_lookup_legacy_only (&legacy_err);
        if (legacy_err != NULL)
            g_clear_error (&legacy_err);
        if (legacy_pwd != NULL)
        {
            self->migrating_legacy_keyring = TRUE;
            on_password_received (NULL, legacy_pwd, NULL, self);
            secret_password_free (legacy_pwd);
            return;
        }
    }

    /* password == NULL: distinguish "not stored" (err == NULL, normal) from
     * "keyring is broken" (err != NULL). For the broken case, disable the
     * setting so we don't keep hitting the same failure on every launch
     * (issue #446). In both cases, fall through to the password dialog so
     * the user can still unlock. */
    if (err != NULL)
    {
        application_disable_secret_service_runtime (self, err->message);
        g_clear_error (&err);
    }

    self->migrating_legacy_keyring = FALSE;
    if (self->window != NULL)
    {
        PasswordDialog *dlg = password_dialog_new (PASSWORD_MODE_DECRYPT,
                                                   on_password_received,
                                                   self);
        lock_app_install_unlock_dialog_quit (dlg, self);
        adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self->window));
    }
}

/* Issue #448: pre-5.0 stored the keyring password under the fixed
 * "main_pwd" attribute and shipped secret-service=TRUE by default. v5
 * keys keyring entries by absolute db_path and defaults secret-service to
 * FALSE. Upgraders therefore lose auto-unlock and look at a fresh password
 * prompt with no idea what changed. On the first launch where the
 * migration has not yet run, probe the keyring for a legacy entry; if one
 * exists and the user has not explicitly chosen a value for the
 * secret-service setting, flip it ON so the per-launch fallback in
 * on_secret_lookup_done can recover the password. Always set the flag
 * afterward so we never probe twice on the same profile. */
static void
maybe_migrate_v4_secret_service (OTPClientApplication *self)
{
    if (self->settings == NULL)
        return;
    if (g_settings_get_boolean (self->settings, "secret-service-v4-migrated"))
        return;

    GVariant *user_value = g_settings_get_user_value (self->settings, "secret-service");
    gboolean inheriting_default = (user_value == NULL);
    if (user_value != NULL)
        g_variant_unref (user_value);

    if (inheriting_default && otpclient_secret_legacy_entry_exists ()) {
        g_message ("Detected pre-5.0 secret-service entry; auto-enabling Secret Service integration");
        otpclient_application_set_use_secret_service (self, TRUE);
    }

    g_settings_set_boolean (self->settings, "secret-service-v4-migrated", TRUE);
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

    /* Validate every entry against the filesystem so missing files surface
     * in the sidebar (warning icon + dim style). The sidebar's db_store
     * holds different DatabaseEntry objects than db_list — check the
     * sidebar's copy of the primary entry below. */
    GListStore *sidebar_store = otpclient_window_get_db_store (self->window);
    gui_misc_validate_databases (sidebar_store);

    g_autoptr (DatabaseEntry) primary_sidebar_entry = g_list_model_get_item (
        G_LIST_MODEL (sidebar_store), (guint)primary_index);
    if (primary_sidebar_entry != NULL && database_entry_get_missing (primary_sidebar_entry))
    {
        present_db_missing_dialog (self->window,
                                   database_entry_get_name (primary_sidebar_entry),
                                   database_entry_get_path (primary_sidebar_entry));
        return;
    }

    /* Set up the primary database for decryption */
    DatabaseEntry *primary_entry = g_ptr_array_index (db_list, primary_index);
    const gchar *db_path = database_entry_get_path (primary_entry);

    self->db_data = database_data_new (db_path, memlock_value);

    maybe_migrate_v4_secret_service (self);

    if (self->use_secret_service)
    {
        secret_password_lookup (OTPCLIENT_SCHEMA, self->cancellable,
                                on_secret_lookup_done,
                                application_async_context_new (self),
                                "string", self->db_data->db_path,
                                NULL);
    }
    else
    {
        PasswordDialog *dlg = password_dialog_new (PASSWORD_MODE_DECRYPT,
                                                   on_password_received,
                                                   self);
        lock_app_install_unlock_dialog_quit (dlg, self);
        adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self->window));
    }
}

static void
clear_session_clipboard (void)
{
    /* Best-effort: requires a default GdkDisplay, which is only present after
     * the app activates. Outside that window the clipboard is naturally empty. */
    GdkDisplay *display = gdk_display_get_default ();
    if (display == NULL)
        return;
    GdkClipboard *clipboard = gdk_display_get_clipboard (display);
    if (clipboard != NULL)
        gdk_clipboard_set_text (clipboard, "");
}

static gboolean
otpclient_application_signal_quit (gpointer user_data)
{
    GApplication *app = G_APPLICATION (user_data);
    OTPClientApplication *self = OTPCLIENT_APPLICATION (app);
    /* Persist any deferred HOTP counter advances before bailing — otherwise
     * a SIGTERM (e.g. from the session manager during logout) loses them
     * and the next startup serves stale codes. */
    if (self->window != NULL)
        otpclient_window_flush_pending_writes (self->window);
    clear_session_clipboard ();
    g_application_quit (app);
    return G_SOURCE_REMOVE;
}

static void
otpclient_application_shutdown (GApplication *application)
{
    /* Covers the normal-exit case (Quit action, window close, app.quit). The
     * signal path goes through clear_session_clipboard before calling quit,
     * but doing it here too is idempotent. */
    OTPClientApplication *self = OTPCLIENT_APPLICATION (application);
    if (self->window != NULL)
        otpclient_window_flush_pending_writes (self->window);
    clear_session_clipboard ();
    G_APPLICATION_CLASS (otpclient_application_parent_class)->shutdown (application);
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
        self->search_provider_keyword = g_settings_get_string (self->settings, "search-provider-keyword");
        self->show_validity_seconds = g_settings_get_boolean (self->settings, "show-validity-seconds");
        self->validity_color = g_settings_get_string (self->settings, "validity-color");
        self->validity_warning_color = g_settings_get_string (self->settings, "validity-warning-color");
        self->minimize_to_tray = g_settings_get_boolean (self->settings, "minimize-to-tray");
        self->clipboard_clear_timeout = g_settings_get_uint (self->settings, "clipboard-clear-timeout");
        self->hide_otps = g_settings_get_boolean (self->settings, "hide-otps");
    } else {
        self->settings = NULL;
        self->show_next_otp = FALSE;
        self->disable_notifications = FALSE;
        self->auto_lock = FALSE;
        self->inactivity_timeout = 0;
        self->use_dark_theme = FALSE;
        self->use_secret_service = FALSE;
        self->search_provider_enabled = TRUE;
        self->search_provider_keyword = g_strdup ("otp");
        self->show_validity_seconds = FALSE;
        self->validity_color = g_strdup ("#008000");
        self->validity_warning_color = g_strdup ("#ffa500");
        self->minimize_to_tray = FALSE;
        self->clipboard_clear_timeout = 30;
        self->hide_otps = TRUE;
    }

    G_APPLICATION_CLASS (otpclient_application_parent_class)->startup (application);

    /* Apply dark theme preference. Must run after the parent startup chain so
     * that GTK/GDK has been initialized; otherwise adw_style_manager_ensure()
     * calls gdk_display_manager_get() before gtk_init() and aborts (issue #440). */
    if (self->use_dark_theme)
        adw_style_manager_set_color_scheme (adw_style_manager_get_default (),
                                            ADW_COLOR_SCHEME_FORCE_DARK);

    /* Clear the clipboard and exit cleanly on signal-driven termination so
     * we don't leave a copied OTP behind when the user kills the process. */
    g_unix_signal_add (SIGINT,  otpclient_application_signal_quit, application);
    g_unix_signal_add (SIGTERM, otpclient_application_signal_quit, application);
    g_unix_signal_add (SIGHUP,  otpclient_application_signal_quit, application);

    gtk_window_set_default_icon_name (APPLICATION_ID);
    self->window = OTPCLIENT_WINDOW(otpclient_window_new (self));
    /* GTK owns the window; we just hold an observer pointer. The weak ref
     * gets self->window cleared to NULL on finalize, so signal_quit() and
     * shutdown() (which both call flush_pending_writes via this pointer)
     * don't dereference a dangling object after an X-button close. */
    g_object_add_weak_pointer (G_OBJECT (self->window), (gpointer *) &self->window);

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
    g_clear_pointer (&self->search_provider_keyword, g_free);
    g_clear_pointer (&self->validity_color, g_free);
    g_clear_pointer (&self->validity_warning_color, g_free);

    if (self->db_data != NULL)
    {
        database_data_free (self->db_data);
        self->db_data = NULL;
    }

    G_OBJECT_CLASS (otpclient_application_parent_class)->dispose (object);
}

static void
otpclient_application_get_property (GObject    *object,
                                    guint       prop_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
    OTPClientApplication *self = OTPCLIENT_APPLICATION (object);
    switch (prop_id) {
        case PROP_HIDE_OTPS:
            g_value_set_boolean (value, self->hide_otps);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
otpclient_application_set_property (GObject      *object,
                                    guint         prop_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
    OTPClientApplication *self = OTPCLIENT_APPLICATION (object);
    switch (prop_id) {
        case PROP_HIDE_OTPS:
            otpclient_application_set_hide_otps (self, g_value_get_boolean (value));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
otpclient_application_class_init (OTPClientApplicationClass *klass)
{
    GApplicationClass *application_class = G_APPLICATION_CLASS(klass);
    GObjectClass *object_class = G_OBJECT_CLASS(klass);

    application_class->activate = otpclient_application_activate;
    application_class->startup = otpclient_application_startup;
    application_class->shutdown = otpclient_application_shutdown;
    object_class->dispose = otpclient_application_dispose;
    object_class->get_property = otpclient_application_get_property;
    object_class->set_property = otpclient_application_set_property;

    /* Notifies on every change so the main window can re-render OTP cells
     * without a restart when the user toggles the setting from preferences. */
    properties[PROP_HIDE_OTPS] =
        g_param_spec_boolean ("hide-otps", NULL, NULL, TRUE,
                              G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

    g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
otpclient_application_init (OTPClientApplication *self)
{
    self->db_data = NULL;
    self->cancellable = g_cancellable_new ();
    self->settings = NULL;
    self->search_provider_keyword = NULL;
    self->validity_color = NULL;
    self->validity_warning_color = NULL;
}

DatabaseData *
otpclient_application_get_db_data (OTPClientApplication *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_APPLICATION (self), NULL);
    return self->db_data;
}

gboolean
otpclient_application_is_unlocking (OTPClientApplication *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_APPLICATION (self), FALSE);
    return self->unlock_in_progress;
}

void
otpclient_application_set_db_data (OTPClientApplication *self,
                                    DatabaseData         *db_data)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (self));

    database_data_free (self->db_data);

    self->db_data = db_data;
}

void
otpclient_application_switch_to_db (OTPClientApplication *self,
                                    const gchar          *db_path)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (self));
    g_return_if_fail (db_path != NULL && db_path[0] != '\0');

    if (self->db_data != NULL && g_strcmp0 (self->db_data->db_path, db_path) == 0)
        return;

    /* Persist any deferred HOTP writes from the outgoing DB while the
     * key is still in memory, then tear the window state down. */
    if (self->window != NULL)
    {
        otpclient_window_flush_pending_writes (self->window);
        otpclient_window_stop_otp_timer (self->window);
        otpclient_window_clear_clipboard_now (self->window);
        otpclient_window_invalidate_cross_db (self->window);

        GListStore *store = otpclient_window_get_otp_store (self->window);
        if (store != NULL)
            g_list_store_remove_all (store);
        otpclient_window_set_db_actions_enabled (self->window, FALSE);
    }

    otpclient_application_set_db_data (self, NULL);

    gint32 memlock_value = 0;
    if (set_memlock_value (&memlock_value) == MEMLOCK_ERR)
        memlock_value = DEFAULT_MEMLOCK_VALUE;

    self->db_data = database_data_new (db_path, memlock_value);

    if (self->use_secret_service)
    {
        secret_password_lookup (OTPCLIENT_SCHEMA, self->cancellable,
                                on_secret_lookup_done,
                                application_async_context_new (self),
                                "string", db_path,
                                NULL);
    }
    else if (self->window != NULL)
    {
        PasswordDialog *dlg = password_dialog_new (PASSWORD_MODE_DECRYPT,
                                                   on_password_received,
                                                   self);
        lock_app_install_unlock_dialog_quit (dlg, self);
        adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self->window));
    }
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

const gchar *otpclient_application_get_search_provider_keyword (OTPClientApplication *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_APPLICATION (self), "otp");
    return self->search_provider_keyword;
}

void otpclient_application_set_search_provider_keyword (OTPClientApplication *self, const gchar *keyword)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (self));
    g_free (self->search_provider_keyword);
    self->search_provider_keyword = g_strdup (keyword ? keyword : "");
    if (self->settings != NULL)
        g_settings_set_string (self->settings, "search-provider-keyword", self->search_provider_keyword);
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
    g_free (self->search_provider_keyword);
    self->search_provider_keyword = g_settings_get_string (self->settings, "search-provider-keyword");
    self->show_validity_seconds = g_settings_get_boolean (self->settings, "show-validity-seconds");
    g_free (self->validity_color);
    self->validity_color = g_settings_get_string (self->settings, "validity-color");
    g_free (self->validity_warning_color);
    self->validity_warning_color = g_settings_get_string (self->settings, "validity-warning-color");
    self->minimize_to_tray = g_settings_get_boolean (self->settings, "minimize-to-tray");
    self->clipboard_clear_timeout = g_settings_get_uint (self->settings, "clipboard-clear-timeout");
    gboolean old_hide_otps = self->hide_otps;
    self->hide_otps = g_settings_get_boolean (self->settings, "hide-otps");
    if (old_hide_otps != self->hide_otps)
        g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_HIDE_OTPS]);

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

gboolean otpclient_application_get_hide_otps (OTPClientApplication *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_APPLICATION (self), TRUE);
    return self->hide_otps;
}

void otpclient_application_set_hide_otps (OTPClientApplication *self, gboolean hide)
{
    g_return_if_fail (OTPCLIENT_IS_APPLICATION (self));
    if (self->hide_otps == hide)
        return;
    self->hide_otps = hide;
    if (self->settings != NULL)
        g_settings_set_boolean (self->settings, "hide-otps", hide);
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_HIDE_OTPS]);
}
