#pragma once

#include <glib-object.h>

G_BEGIN_DECLS

#define OTP_TYPE_ENTRY (otp_entry_get_type ())

G_DECLARE_FINAL_TYPE (OTPEntry, otp_entry, OTP, ENTRY, GObject)

OTPEntry   *otp_entry_new          (const gchar *account,
                                    const gchar *issuer,
                                    const gchar *otp_value,
                                    const gchar *otp_type,
                                    guint32      period,
                                    guint64      counter,
                                    const gchar *algorithm,
                                    guint32      digits,
                                    const gchar *secret);

const gchar *otp_entry_get_account  (OTPEntry *self);
const gchar *otp_entry_get_issuer   (OTPEntry *self);
const gchar *otp_entry_get_otp_value(OTPEntry *self);
const gchar *otp_entry_get_otp_type (OTPEntry *self);
guint32      otp_entry_get_period   (OTPEntry *self);
guint64      otp_entry_get_counter  (OTPEntry *self);
const gchar *otp_entry_get_algorithm(OTPEntry *self);
guint32      otp_entry_get_digits   (OTPEntry *self);

void         otp_entry_set_account  (OTPEntry    *self,
                                     const gchar *account);
void         otp_entry_set_issuer   (OTPEntry    *self,
                                     const gchar *issuer);
void         otp_entry_set_otp_value(OTPEntry    *self,
                                     const gchar *otp_value);
void         otp_entry_set_counter  (OTPEntry    *self,
                                     guint64      counter);

void         otp_entry_set_db_name  (OTPEntry    *self,
                                     const gchar *db_name);
const gchar *otp_entry_get_db_name (OTPEntry *self);

void         otp_entry_set_group   (OTPEntry    *self,
                                     const gchar *group);
const gchar *otp_entry_get_group   (OTPEntry *self);

void         otp_entry_update_otp   (OTPEntry *self);

gchar       *otp_entry_get_next_otp (OTPEntry *self);

G_END_DECLS
