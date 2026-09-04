#pragma once

#include "otpclient-types.h"
#include <adwaita.h>

typedef struct db_data_t DatabaseData;

G_BEGIN_DECLS

#define OTPCLIENT_TYPE_APPLICATION (otpclient_application_get_type())

G_DECLARE_FINAL_TYPE (OTPClientApplication, otpclient_application, OTPCLIENT, APPLICATION, AdwApplication)

OTPClientApplication *otpclient_application_new      (void);

/* The main window, or NULL once GTK has destroyed it. Backed by the weak
 * pointer set in startup(), so this doubles as a liveness check. Prefer it over
 * gtk_application_get_active_window(), which reports whatever is focused and so
 * can hand back a transient dialog, or NULL while the window is hidden. */
GtkWindow            *otpclient_application_get_window  (OTPClientApplication *self);

DatabaseData         *otpclient_application_get_db_data (OTPClientApplication *self);
void                  otpclient_application_set_db_data (OTPClientApplication *self,
                                                         DatabaseData         *db_data);

void                  otpclient_application_switch_to_db (OTPClientApplication *self,
                                                          const gchar          *db_path);

/* Move the keyring entry of the database that is currently open from the path
 * it used to live at to the one it lives at now. The keyring is keyed by
 * absolute path, so a database that moved otherwise keeps its password under a
 * key nothing looks up again: automatic unlock quietly stops working and the
 * old entry lingers. Call after a relocation has been loaded successfully, so
 * that the key in db_data is one the database has actually accepted.
 *
 * Does nothing unless Secret Service is enabled, and clears the old entry only
 * once the new one is stored. */
void                  otpclient_application_relocate_stored_password (OTPClientApplication *self,
                                                                      const gchar          *old_db_path);

/* TRUE between on_password_received handing db_data to the unlock worker
 * and on_unlock_done firing. Window callers must consult this before any
 * action that would free or replace db_data: the worker holds a raw
 * pointer to it and freeing under its feet is a use-after-free. */
gboolean              otpclient_application_is_unlocking (OTPClientApplication *self);

/* TRUE when a database is loaded and the app is not locked, i.e. OTP data is
 * currently accessible. Used to tell a "successful unlock" dialog close apart
 * from a user dismissal. */
gboolean              otpclient_application_is_db_unlocked (OTPClientApplication *self);
gboolean              otpclient_application_submit_unlock_password (
                                                           OTPClientApplication *self,
                                                           const gchar          *password,
                                                           gchar               **error_message);
void                  otpclient_application_purge_secrets (OTPClientApplication *self);

gboolean              otpclient_application_get_show_next_otp (OTPClientApplication *self);
void                  otpclient_application_set_show_next_otp (OTPClientApplication *self,
                                                               gboolean              show);

gboolean              otpclient_application_get_disable_notifications (OTPClientApplication *self);
void                  otpclient_application_set_disable_notifications (OTPClientApplication *self,
                                                                       gboolean              disable);

gboolean              otpclient_application_get_auto_lock (OTPClientApplication *self);
void                  otpclient_application_set_auto_lock (OTPClientApplication *self,
                                                           gboolean              auto_lock);

gint                  otpclient_application_get_inactivity_timeout (OTPClientApplication *self);
void                  otpclient_application_set_inactivity_timeout (OTPClientApplication *self,
                                                                    gint                  timeout);

gboolean              otpclient_application_get_app_locked (OTPClientApplication *self);
void                  otpclient_application_set_app_locked (OTPClientApplication *self,
                                                            gboolean              locked);

gboolean              otpclient_application_get_use_dark_theme (OTPClientApplication *self);
void                  otpclient_application_set_use_dark_theme (OTPClientApplication *self,
                                                                gboolean              use_dark);

gboolean              otpclient_application_get_use_secret_service (OTPClientApplication *self);
void                  otpclient_application_set_use_secret_service (OTPClientApplication *self,
                                                                    gboolean              use_ss);

gboolean              otpclient_application_get_search_provider_enabled (OTPClientApplication *self);
void                  otpclient_application_set_search_provider_enabled (OTPClientApplication *self,
                                                                         gboolean              enabled);

const gchar          *otpclient_application_get_search_provider_keyword (OTPClientApplication *self);
void                  otpclient_application_set_search_provider_keyword (OTPClientApplication *self,
                                                                         const gchar          *keyword);

gboolean              otpclient_application_get_show_validity_seconds (OTPClientApplication *self);
void                  otpclient_application_set_show_validity_seconds (OTPClientApplication *self,
                                                                       gboolean              show);

const gchar          *otpclient_application_get_validity_color (OTPClientApplication *self);
void                  otpclient_application_set_validity_color (OTPClientApplication *self,
                                                                const gchar          *color);

const gchar          *otpclient_application_get_validity_warning_color (OTPClientApplication *self);
void                  otpclient_application_set_validity_warning_color (OTPClientApplication *self,
                                                                        const gchar          *color);

gboolean              otpclient_application_get_minimize_to_tray (OTPClientApplication *self);
void                  otpclient_application_set_minimize_to_tray (OTPClientApplication *self,
                                                                   gboolean              minimize);

/* Turning minimize-to-tray off also turns this off: hiding a window with no
 * tray to hide it in is a window that never comes back. */
gboolean              otpclient_application_get_start_minimized (OTPClientApplication *self);
void                  otpclient_application_set_start_minimized (OTPClientApplication *self,
                                                                 gboolean              minimized);

gboolean              otpclient_application_get_autostart (OTPClientApplication *self);
void                  otpclient_application_set_autostart (OTPClientApplication *self,
                                                           gboolean              autostart);

/* The durable "the desktop has not been told yet" note. Set by anything that
 * writes the startup keys without being able to act on them, which is what a
 * settings import does, and retired only by a reconciliation that actually got
 * an answer. It is what makes the next launch ask again, and under Flatpak it
 * is the only thing that can, since the login entry lives outside the sandbox
 * and cannot be read back. */
gboolean              otpclient_application_get_startup_reconcile_pending (OTPClientApplication *self);
void                  otpclient_application_set_startup_reconcile_pending (OTPClientApplication *self,
                                                                           gboolean              pending);

guint                 otpclient_application_get_clipboard_clear_timeout (OTPClientApplication *self);
void                  otpclient_application_set_clipboard_clear_timeout (OTPClientApplication *self,
                                                                         guint                 timeout);

gboolean              otpclient_application_get_hide_otps (OTPClientApplication *self);
void                  otpclient_application_set_hide_otps (OTPClientApplication *self,
                                                           gboolean              hide);

void                  otpclient_application_reload_settings (OTPClientApplication *self);
void                  otpclient_application_reconcile_startup_settings (OTPClientApplication *self);

/* Say that a startup setting was taken back because the desktop would not have
 * it. Called from the autostart reconciliation, which is the one place that
 * learns of a refusal and runs whether or not there is a window to tell; with
 * no window the message is dropped, since the switches read the keys anyway. */
void                  otpclient_application_report_startup_change (OTPClientApplication *self,
                                                                   const gchar          *message);

/* The one way the window becomes visible: activate(), the tray's Show item and
 * its Activate method, the tray's give-up paths, and the started-hidden
 * deadline all come through here. Anything a normal startup does after showing
 * the window belongs here too, or a start-minimized launch silently skips it. */
void                  otpclient_application_present_window (OTPClientApplication *self);

G_END_DECLS
