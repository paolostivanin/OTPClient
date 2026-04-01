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

G_END_DECLS
