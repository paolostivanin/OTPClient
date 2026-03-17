#include <glib/gi18n.h>
#include <gcrypt.h>
#include <cotp.h>
#include "otp-entry.h"

struct _OTPEntry
{
    GObject parent_instance;

    gchar *account;
    gchar *issuer;
    gchar *otp_value;
    gchar *otp_type;     /* "TOTP" or "HOTP" */
    guint32 period;
    guint64 counter;
    gchar *algorithm;    /* "SHA1", "SHA256", "SHA512" */
    guint32 digits;
    gchar *secret;       /* base32-encoded, held in gcrypt secure memory */
};

enum
{
    PROP_0,
    PROP_ACCOUNT,
    PROP_ISSUER,
    PROP_OTP_VALUE,
    PROP_OTP_TYPE,
    PROP_PERIOD,
    PROP_COUNTER,
    PROP_ALGORITHM,
    PROP_DIGITS,
    PROP_SECRET,
    N_PROPS
};

static GParamSpec *properties[N_PROPS];

G_DEFINE_FINAL_TYPE (OTPEntry, otp_entry, G_TYPE_OBJECT)

static gint
get_algo_int (const gchar *algo)
{
    if (g_strcmp0 (algo, "SHA256") == 0)
        return COTP_SHA256;
    if (g_strcmp0 (algo, "SHA512") == 0)
        return COTP_SHA512;
    return COTP_SHA1;
}

static void
otp_entry_finalize (GObject *object)
{
    OTPEntry *self = OTP_ENTRY (object);

    g_clear_pointer (&self->account, g_free);
    g_clear_pointer (&self->issuer, g_free);
    g_clear_pointer (&self->otp_value, g_free);
    g_clear_pointer (&self->otp_type, g_free);
    g_clear_pointer (&self->algorithm, g_free);

    if (self->secret != NULL)
    {
        gcry_free (self->secret);
        self->secret = NULL;
    }

    G_OBJECT_CLASS (otp_entry_parent_class)->finalize (object);
}

static void
otp_entry_get_property (GObject    *object,
                        guint       prop_id,
                        GValue     *value,
                        GParamSpec *pspec)
{
    OTPEntry *self = OTP_ENTRY (object);

    switch (prop_id)
    {
        case PROP_ACCOUNT:
            g_value_set_string (value, self->account);
            break;
        case PROP_ISSUER:
            g_value_set_string (value, self->issuer);
            break;
        case PROP_OTP_VALUE:
            g_value_set_string (value, self->otp_value);
            break;
        case PROP_OTP_TYPE:
            g_value_set_string (value, self->otp_type);
            break;
        case PROP_PERIOD:
            g_value_set_uint (value, self->period);
            break;
        case PROP_COUNTER:
            g_value_set_uint64 (value, self->counter);
            break;
        case PROP_ALGORITHM:
            g_value_set_string (value, self->algorithm);
            break;
        case PROP_DIGITS:
            g_value_set_uint (value, self->digits);
            break;
        case PROP_SECRET:
            g_value_set_string (value, self->secret);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
otp_entry_set_property (GObject      *object,
                        guint         prop_id,
                        const GValue *value,
                        GParamSpec   *pspec)
{
    OTPEntry *self = OTP_ENTRY (object);

    switch (prop_id)
    {
        case PROP_ACCOUNT:
            g_free (self->account);
            self->account = g_value_dup_string (value);
            break;
        case PROP_ISSUER:
            g_free (self->issuer);
            self->issuer = g_value_dup_string (value);
            break;
        case PROP_OTP_VALUE:
            g_free (self->otp_value);
            self->otp_value = g_value_dup_string (value);
            break;
        case PROP_OTP_TYPE:
            g_free (self->otp_type);
            self->otp_type = g_value_dup_string (value);
            break;
        case PROP_PERIOD:
            self->period = g_value_get_uint (value);
            break;
        case PROP_COUNTER:
            self->counter = g_value_get_uint64 (value);
            break;
        case PROP_ALGORITHM:
            g_free (self->algorithm);
            self->algorithm = g_value_dup_string (value);
            break;
        case PROP_DIGITS:
            self->digits = g_value_get_uint (value);
            break;
        case PROP_SECRET:
            if (self->secret != NULL)
                gcry_free (self->secret);
            {
                const gchar *str = g_value_get_string (value);
                if (str != NULL)
                {
                    self->secret = gcry_calloc_secure (strlen (str) + 1, 1);
                    memcpy (self->secret, str, strlen (str) + 1);
                }
                else
                {
                    self->secret = NULL;
                }
            }
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
otp_entry_class_init (OTPEntryClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = otp_entry_finalize;
    object_class->get_property = otp_entry_get_property;
    object_class->set_property = otp_entry_set_property;

    properties[PROP_ACCOUNT] =
        g_param_spec_string ("account", NULL, NULL, NULL,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
    properties[PROP_ISSUER] =
        g_param_spec_string ("issuer", NULL, NULL, NULL,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
    properties[PROP_OTP_VALUE] =
        g_param_spec_string ("otp-value", NULL, NULL, NULL,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
    properties[PROP_OTP_TYPE] =
        g_param_spec_string ("otp-type", NULL, NULL, "TOTP",
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    properties[PROP_PERIOD] =
        g_param_spec_uint ("period", NULL, NULL, 1, 300, 30,
                           G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    properties[PROP_COUNTER] =
        g_param_spec_uint64 ("counter", NULL, NULL, 0, G_MAXUINT64, 0,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
    properties[PROP_ALGORITHM] =
        g_param_spec_string ("algorithm", NULL, NULL, "SHA1",
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    properties[PROP_DIGITS] =
        g_param_spec_uint ("digits", NULL, NULL, 4, 10, 6,
                           G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
    properties[PROP_SECRET] =
        g_param_spec_string ("secret", NULL, NULL, NULL,
                             G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);

    g_object_class_install_properties (object_class, N_PROPS, properties);
}

static void
otp_entry_init (OTPEntry *self)
{
    self->period = 30;
    self->digits = 6;
    self->algorithm = g_strdup ("SHA1");
    self->otp_type = g_strdup ("TOTP");
}

OTPEntry *
otp_entry_new (const gchar *account,
               const gchar *issuer,
               const gchar *otp_value,
               const gchar *otp_type,
               guint32      period,
               guint64      counter,
               const gchar *algorithm,
               guint32      digits,
               const gchar *secret)
{
    OTPEntry *self = g_object_new (OTP_TYPE_ENTRY,
                                   "account", account,
                                   "issuer", issuer,
                                   "otp-type", otp_type,
                                   "period", period,
                                   "counter", counter,
                                   "algorithm", algorithm,
                                   "digits", digits,
                                   "secret", secret,
                                   NULL);

    if (otp_value != NULL)
    {
        g_free (self->otp_value);
        self->otp_value = g_strdup (otp_value);
    }

    return self;
}

const gchar *
otp_entry_get_account (OTPEntry *self)
{
    g_return_val_if_fail (OTP_IS_ENTRY (self), NULL);
    return self->account;
}

const gchar *
otp_entry_get_issuer (OTPEntry *self)
{
    g_return_val_if_fail (OTP_IS_ENTRY (self), NULL);
    return self->issuer;
}

const gchar *
otp_entry_get_otp_value (OTPEntry *self)
{
    g_return_val_if_fail (OTP_IS_ENTRY (self), NULL);
    return self->otp_value;
}

const gchar *
otp_entry_get_otp_type (OTPEntry *self)
{
    g_return_val_if_fail (OTP_IS_ENTRY (self), NULL);
    return self->otp_type;
}

guint32
otp_entry_get_period (OTPEntry *self)
{
    g_return_val_if_fail (OTP_IS_ENTRY (self), 30);
    return self->period;
}

guint64
otp_entry_get_counter (OTPEntry *self)
{
    g_return_val_if_fail (OTP_IS_ENTRY (self), 0);
    return self->counter;
}

const gchar *
otp_entry_get_algorithm (OTPEntry *self)
{
    g_return_val_if_fail (OTP_IS_ENTRY (self), NULL);
    return self->algorithm;
}

guint32
otp_entry_get_digits (OTPEntry *self)
{
    g_return_val_if_fail (OTP_IS_ENTRY (self), 6);
    return self->digits;
}

void
otp_entry_set_account (OTPEntry    *self,
                       const gchar *account)
{
    g_return_if_fail (OTP_IS_ENTRY (self));

    if (g_strcmp0 (self->account, account) == 0)
        return;

    g_free (self->account);
    self->account = g_strdup (account);
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ACCOUNT]);
}

void
otp_entry_set_issuer (OTPEntry    *self,
                      const gchar *issuer)
{
    g_return_if_fail (OTP_IS_ENTRY (self));

    if (g_strcmp0 (self->issuer, issuer) == 0)
        return;

    g_free (self->issuer);
    self->issuer = g_strdup (issuer);
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_ISSUER]);
}

void
otp_entry_set_otp_value (OTPEntry    *self,
                         const gchar *otp_value)
{
    g_return_if_fail (OTP_IS_ENTRY (self));

    if (g_strcmp0 (self->otp_value, otp_value) == 0)
        return;

    g_free (self->otp_value);
    self->otp_value = g_strdup (otp_value);
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_OTP_VALUE]);
}

void
otp_entry_set_counter (OTPEntry *self,
                       guint64   counter)
{
    g_return_if_fail (OTP_IS_ENTRY (self));

    if (self->counter == counter)
        return;

    self->counter = counter;
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_COUNTER]);
}

void
otp_entry_update_otp (OTPEntry *self)
{
    g_return_if_fail (OTP_IS_ENTRY (self));

    if (self->secret == NULL || self->secret[0] == '\0')
        return;

    cotp_error_t err;
    gint algo = get_algo_int (self->algorithm);
    gchar *otp = NULL;

    if (g_ascii_strcasecmp (self->otp_type, "TOTP") == 0)
    {
        otp = get_totp (self->secret, self->digits, self->period, algo, &err);
    }
    else
    {
        otp = get_hotp (self->secret, self->counter, self->digits, algo, &err);
    }

    if (otp != NULL && err == NO_ERROR)
    {
        otp_entry_set_otp_value (self, otp);
        g_free (otp);
    }
    else
    {
        otp_entry_set_otp_value (self, _("Error"));
        g_free (otp);
    }
}

gchar *
otp_entry_get_next_otp (OTPEntry *self)
{
    g_return_val_if_fail (OTP_IS_ENTRY (self), NULL);

    if (self->secret == NULL || self->secret[0] == '\0')
        return NULL;

    if (g_ascii_strcasecmp (self->otp_type, "TOTP") != 0)
        return NULL;

    cotp_error_t err;
    gint algo = get_algo_int (self->algorithm);

    gint64 now = g_get_real_time () / G_USEC_PER_SEC;
    gint64 next_step_time = now + self->period - (now % self->period);

    gchar *otp = get_totp_at (self->secret, next_step_time, self->digits, self->period, algo, &err);
    if (otp != NULL && err == NO_ERROR)
        return otp;

    g_free (otp);
    return NULL;
}
