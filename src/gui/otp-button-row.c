#include "otp-button-row.h"

struct _OtpButtonRow
{
    AdwPreferencesRow parent_instance;

    GtkWidget *box;
    GtkWidget *start_image;
    GtkWidget *label;
    GtkWidget *end_image;

    gchar *text;
    gchar *start_icon_name;
    gchar *end_icon_name;
};

enum
{
    PROP_0,
    PROP_TEXT,
    PROP_START_ICON_NAME,
    PROP_END_ICON_NAME,
    N_PROPS
};

enum
{
    SIGNAL_ACTIVATED,
    N_SIGNALS
};

static GParamSpec *properties[N_PROPS];
static guint       signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (OtpButtonRow, otp_button_row, ADW_TYPE_PREFERENCES_ROW)

static void
ensure_css_provider (void)
{
    static gsize loaded = 0;
    if (g_once_init_enter (&loaded))
    {
        GtkCssProvider *provider = gtk_css_provider_new ();
        gtk_css_provider_load_from_resource (provider,
                                             "/com/github/paolostivanin/OTPClient/ui/otp-button-row.css");
        gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                                    GTK_STYLE_PROVIDER (provider),
                                                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref (provider);
        g_once_init_leave (&loaded, 1);
    }
}

static void
update_start_icon (OtpButtonRow *self)
{
    if (self->start_icon_name != NULL && self->start_icon_name[0] != '\0')
    {
        gtk_image_set_from_icon_name (GTK_IMAGE (self->start_image), self->start_icon_name);
        gtk_widget_set_visible (self->start_image, TRUE);
    }
    else
    {
        gtk_widget_set_visible (self->start_image, FALSE);
    }
}

static void
update_end_icon (OtpButtonRow *self)
{
    if (self->end_icon_name != NULL && self->end_icon_name[0] != '\0')
    {
        gtk_image_set_from_icon_name (GTK_IMAGE (self->end_image), self->end_icon_name);
        gtk_widget_set_visible (self->end_image, TRUE);
    }
    else
    {
        gtk_widget_set_visible (self->end_image, FALSE);
    }
}

static void
otp_button_row_activate_impl (GtkListBoxRow *row)
{
    g_signal_emit (row, signals[SIGNAL_ACTIVATED], 0);
}

static void
otp_button_row_finalize (GObject *object)
{
    OtpButtonRow *self = OTP_BUTTON_ROW (object);

    g_clear_pointer (&self->text, g_free);
    g_clear_pointer (&self->start_icon_name, g_free);
    g_clear_pointer (&self->end_icon_name, g_free);

    G_OBJECT_CLASS (otp_button_row_parent_class)->finalize (object);
}

static void
otp_button_row_get_property (GObject    *object,
                             guint       prop_id,
                             GValue     *value,
                             GParamSpec *pspec)
{
    OtpButtonRow *self = OTP_BUTTON_ROW (object);

    switch (prop_id)
    {
        case PROP_TEXT:
            g_value_set_string (value, self->text);
            break;
        case PROP_START_ICON_NAME:
            g_value_set_string (value, self->start_icon_name);
            break;
        case PROP_END_ICON_NAME:
            g_value_set_string (value, self->end_icon_name);
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
otp_button_row_set_property (GObject      *object,
                             guint         prop_id,
                             const GValue *value,
                             GParamSpec   *pspec)
{
    OtpButtonRow *self = OTP_BUTTON_ROW (object);

    switch (prop_id)
    {
        case PROP_TEXT:
            otp_button_row_set_text (self, g_value_get_string (value));
            break;
        case PROP_START_ICON_NAME:
            otp_button_row_set_start_icon_name (self, g_value_get_string (value));
            break;
        case PROP_END_ICON_NAME:
            otp_button_row_set_end_icon_name (self, g_value_get_string (value));
            break;
        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
otp_button_row_class_init (OtpButtonRowClass *klass)
{
    GObjectClass      *object_class = G_OBJECT_CLASS (klass);
    GtkListBoxRowClass *row_class   = GTK_LIST_BOX_ROW_CLASS (klass);

    object_class->finalize     = otp_button_row_finalize;
    object_class->get_property = otp_button_row_get_property;
    object_class->set_property = otp_button_row_set_property;

    row_class->activate = otp_button_row_activate_impl;

    properties[PROP_TEXT] =
        g_param_spec_string ("text", NULL, NULL, NULL,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
    properties[PROP_START_ICON_NAME] =
        g_param_spec_string ("start-icon-name", NULL, NULL, NULL,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);
    properties[PROP_END_ICON_NAME] =
        g_param_spec_string ("end-icon-name", NULL, NULL, NULL,
                             G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | G_PARAM_EXPLICIT_NOTIFY);

    g_object_class_install_properties (object_class, N_PROPS, properties);

    signals[SIGNAL_ACTIVATED] =
        g_signal_new ("activated",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 0);
}

static void
otp_button_row_init (OtpButtonRow *self)
{
    ensure_css_provider ();

    gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (self), TRUE);
    gtk_widget_add_css_class (GTK_WIDGET (self), "button-row");

    self->box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class (self->box, "header");

    self->start_image = gtk_image_new ();
    gtk_widget_set_visible (self->start_image, FALSE);
    gtk_box_append (GTK_BOX (self->box), self->start_image);

    self->label = gtk_label_new (NULL);
    gtk_widget_set_hexpand (self->label, TRUE);
    gtk_label_set_xalign (GTK_LABEL (self->label), 0.5);
    gtk_label_set_ellipsize (GTK_LABEL (self->label), PANGO_ELLIPSIZE_END);
    gtk_box_append (GTK_BOX (self->box), self->label);

    self->end_image = gtk_image_new ();
    gtk_widget_set_visible (self->end_image, FALSE);
    gtk_box_append (GTK_BOX (self->box), self->end_image);

    gtk_list_box_row_set_child (GTK_LIST_BOX_ROW (self), self->box);
}

GtkWidget *
otp_button_row_new (void)
{
    return g_object_new (OTP_TYPE_BUTTON_ROW, NULL);
}

const gchar *
otp_button_row_get_text (OtpButtonRow *self)
{
    g_return_val_if_fail (OTP_IS_BUTTON_ROW (self), NULL);
    return self->text;
}

void
otp_button_row_set_text (OtpButtonRow *self,
                         const gchar  *text)
{
    g_return_if_fail (OTP_IS_BUTTON_ROW (self));

    if (g_strcmp0 (self->text, text) == 0)
        return;

    g_free (self->text);
    self->text = g_strdup (text);
    gtk_label_set_label (GTK_LABEL (self->label), text != NULL ? text : "");
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TEXT]);
}

const gchar *
otp_button_row_get_start_icon_name (OtpButtonRow *self)
{
    g_return_val_if_fail (OTP_IS_BUTTON_ROW (self), NULL);
    return self->start_icon_name;
}

void
otp_button_row_set_start_icon_name (OtpButtonRow *self,
                                    const gchar  *icon_name)
{
    g_return_if_fail (OTP_IS_BUTTON_ROW (self));

    if (g_strcmp0 (self->start_icon_name, icon_name) == 0)
        return;

    g_free (self->start_icon_name);
    self->start_icon_name = g_strdup (icon_name);
    update_start_icon (self);
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_START_ICON_NAME]);
}

const gchar *
otp_button_row_get_end_icon_name (OtpButtonRow *self)
{
    g_return_val_if_fail (OTP_IS_BUTTON_ROW (self), NULL);
    return self->end_icon_name;
}

void
otp_button_row_set_end_icon_name (OtpButtonRow *self,
                                  const gchar  *icon_name)
{
    g_return_if_fail (OTP_IS_BUTTON_ROW (self));

    if (g_strcmp0 (self->end_icon_name, icon_name) == 0)
        return;

    g_free (self->end_icon_name);
    self->end_icon_name = g_strdup (icon_name);
    update_end_icon (self);
    g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_END_ICON_NAME]);
}
