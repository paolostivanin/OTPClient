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

/* Cached lowercase variants for fast search filtering. Never NULL. */
const gchar *otp_entry_get_account_lower (OTPEntry *self);
const gchar *otp_entry_get_issuer_lower  (OTPEntry *self);
const gchar *otp_entry_get_group_lower   (OTPEntry *self);

void         otp_entry_update_otp   (OTPEntry *self);

gchar       *otp_entry_get_next_otp (OTPEntry *self);

/* Reveal/hide state controls whether the OTP value column shows the live
 * code or a bullet mask. The state belongs to the entry (not the row widget)
 * so it survives scroll-driven rebinds. notify::revealed fires on change so
 * the bound label can re-render. */
gboolean     otp_entry_get_revealed (OTPEntry *self);
void         otp_entry_set_revealed (OTPEntry    *self,
                                     gboolean     revealed);

/* Convenience: set revealed=TRUE and arm an auto-hide timer. seconds=0 means
 * "reveal until explicitly hidden / lock". Calling again resets the timer. */
void         otp_entry_reveal_for   (OTPEntry    *self,
                                     guint        seconds);

G_END_DECLS
