#pragma once

#include "otpclient-types.h"
#include <adwaita.h>

G_BEGIN_DECLS

#define OTPCLIENT_TYPE_WINDOW (otpclient_window_get_type())

G_DECLARE_FINAL_TYPE (OTPClientWindow, otpclient_window, OTPCLIENT, WINDOW, AdwApplicationWindow)

GtkWidget          *otpclient_window_new            (OTPClientApplication *application);

GListStore         *otpclient_window_get_otp_store  (OTPClientWindow *self);

GtkSingleSelection *otpclient_window_get_otp_selection (OTPClientWindow *self);

void                otpclient_window_start_otp_timer (OTPClientWindow *self);
void                otpclient_window_stop_otp_timer  (OTPClientWindow *self);

void                otpclient_window_add_database    (OTPClientWindow *self,
                                                      const gchar     *name,
                                                      const gchar     *path);
GListStore         *otpclient_window_get_db_store    (OTPClientWindow *self);
gint                otpclient_window_get_selected_db_index (OTPClientWindow *self);

/* Open the file chooser the sidebar's Open button uses, so a caller outside the
 * window can route the user into the same picker -> password -> load pipeline.
 *
 * With replace_path set, the picked database takes over the sidebar entry that
 * currently holds that path instead of being appended as a new one, which is
 * how "Locate..." recovers a database whose file moved or lost its doc id. */
void                otpclient_window_present_open_database (OTPClientWindow *self,
                                                            const gchar     *replace_path);
void                otpclient_window_select_database (OTPClientWindow *self,
                                                      gint             index);

void                otpclient_window_invalidate_cross_db (OTPClientWindow *self);

void                otpclient_window_rebuild_groups  (OTPClientWindow *self);

void                otpclient_window_show_loading    (OTPClientWindow *self);
void                otpclient_window_hide_loading    (OTPClientWindow *self);

void                otpclient_window_show_error_toast (OTPClientWindow *self,
                                                       const gchar     *message);

void                otpclient_window_set_locked_indicator (OTPClientWindow *self,
                                                           gboolean         locked);

void                otpclient_window_set_db_actions_enabled (OTPClientWindow *self,
                                                             gboolean         enabled);

void                otpclient_window_clear_displayed_otps (OTPClientWindow *self);

void                otpclient_window_refresh_content_page (OTPClientWindow *self);

void                otpclient_window_secure_lock_cleanup  (OTPClientWindow *self);

void                otpclient_window_clear_clipboard_now (OTPClientWindow *self);

void                otpclient_window_sync_active_flag (OTPClientWindow *self);

G_END_DECLS
