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

void                otpclient_window_flush_pending_writes (OTPClientWindow *self);

void                otpclient_window_clear_clipboard_now (OTPClientWindow *self);

G_END_DECLS
