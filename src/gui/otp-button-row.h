#pragma once

#include <adwaita.h>

G_BEGIN_DECLS

#if ADW_CHECK_VERSION(1, 6, 0)

typedef AdwButtonRow OtpButtonRow;

#define OTP_TYPE_BUTTON_ROW                 ADW_TYPE_BUTTON_ROW
#define OTP_BUTTON_ROW(obj)                 ADW_BUTTON_ROW(obj)
#define OTP_IS_BUTTON_ROW(obj)              ADW_IS_BUTTON_ROW(obj)

#define otp_button_row_new                  adw_button_row_new
#define otp_button_row_get_start_icon_name  adw_button_row_get_start_icon_name
#define otp_button_row_set_start_icon_name  adw_button_row_set_start_icon_name
#define otp_button_row_get_end_icon_name    adw_button_row_get_end_icon_name
#define otp_button_row_set_end_icon_name    adw_button_row_set_end_icon_name

#else

#define OTP_TYPE_BUTTON_ROW (otp_button_row_get_type ())

G_DECLARE_FINAL_TYPE (OtpButtonRow, otp_button_row, OTP, BUTTON_ROW, AdwPreferencesRow)

GtkWidget   *otp_button_row_new                 (void);

const gchar *otp_button_row_get_start_icon_name (OtpButtonRow *self);
void         otp_button_row_set_start_icon_name (OtpButtonRow *self,
                                                 const gchar  *icon_name);

const gchar *otp_button_row_get_end_icon_name   (OtpButtonRow *self);
void         otp_button_row_set_end_icon_name   (OtpButtonRow *self,
                                                 const gchar  *icon_name);

#endif

G_END_DECLS
