#pragma once

#include "otpclient-types.h"
#include <adwaita.h>

typedef struct db_data_t DatabaseData;

G_BEGIN_DECLS

#define OTPCLIENT_TYPE_APPLICATION (otpclient_application_get_type())

G_DECLARE_FINAL_TYPE (OTPClientApplication, otpclient_application, OTPCLIENT, APPLICATION, AdwApplication)

OTPClientApplication *otpclient_application_new      (void);

DatabaseData         *otpclient_application_get_db_data (OTPClientApplication *self);
void                  otpclient_application_set_db_data (OTPClientApplication *self,
                                                         DatabaseData         *db_data);

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

guint                 otpclient_application_get_clipboard_clear_timeout (OTPClientApplication *self);
void                  otpclient_application_set_clipboard_clear_timeout (OTPClientApplication *self,
                                                                         guint                 timeout);

void                  otpclient_application_reload_settings (OTPClientApplication *self);

G_END_DECLS
