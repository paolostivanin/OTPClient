#pragma once

#define TYPE_TOTP   0x00
#define TYPE_HOTP   0x01

#define ALGO_SHA1   0x02
#define ALGO_SHA256 0x03
#define ALGO_SHA512 0x04


typedef struct _otp_t {
    guint8 type;
    guint8 algo;
    guint8 digits;
    union {
        guint8 period;
        guint8 counter;
    };
    gchar *issuer;
    gchar *label;
    gchar *secret;
} otp_t;


static void free_slist (GSList *otps);