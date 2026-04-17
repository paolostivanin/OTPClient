#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#define OTP_TYPE_BUTTON_ROW (otp_button_row_get_type ())

G_DECLARE_FINAL_TYPE (OtpButtonRow, otp_button_row, OTP, BUTTON_ROW, AdwPreferencesRow)

GtkWidget   *otp_button_row_new                 (void);

const gchar *otp_button_row_get_text            (OtpButtonRow *self);
void         otp_button_row_set_text            (OtpButtonRow *self,
                                                 const gchar  *text);

const gchar *otp_button_row_get_start_icon_name (OtpButtonRow *self);
void         otp_button_row_set_start_icon_name (OtpButtonRow *self,
                                                 const gchar  *icon_name);

const gchar *otp_button_row_get_end_icon_name   (OtpButtonRow *self);
void         otp_button_row_set_end_icon_name   (OtpButtonRow *self,
                                                 const gchar  *icon_name);

G_END_DECLS
