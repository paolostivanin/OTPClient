#include <glib/gi18n.h>
#include <gcrypt.h>
#include "password-dialog.h"

struct _PasswordDialog
{
    AdwDialog parent;

    PasswordDialogMode mode;
    PasswordDialogCallback callback;
    gpointer callback_data;

    GtkWidget *status_page;
    GtkWidget *prefs_group;
    GtkWidget *current_password_row;
    GtkWidget *password_row;
    GtkWidget *confirm_row;
    GtkWidget *unlock_button;
    GtkWidget *error_label;
};

G_DEFINE_FINAL_TYPE (PasswordDialog, password_dialog, ADW_TYPE_DIALOG)

static void
update_unlock_sensitivity (PasswordDialog *self)
{
    const gchar *pwd = gtk_editable_get_text (GTK_EDITABLE (self->password_row));
    gboolean sensitive = (pwd != NULL && pwd[0] != '\0');

    if (self->mode == PASSWORD_MODE_NEW || self->mode == PASSWORD_MODE_CHANGE)
    {
        const gchar *confirm = gtk_editable_get_text (GTK_EDITABLE (self->confirm_row));
        sensitive = sensitive && (confirm != NULL && confirm[0] != '\0');
    }

    if (self->mode == PASSWORD_MODE_CHANGE)
    {
        const gchar *current = gtk_editable_get_text (GTK_EDITABLE (self->current_password_row));
        sensitive = sensitive && (current != NULL && current[0] != '\0');
    }

    gtk_widget_set_sensitive (self->unlock_button, sensitive);
}

static void
on_text_changed (GtkEditable    *editable,
                 PasswordDialog *self)
{
    (void) editable;
    update_unlock_sensitivity (self);
    gtk_widget_set_visible (self->error_label, FALSE);
}

static void
on_unlock_clicked (GtkButton      *button,
                   PasswordDialog *self)
{
    (void) button;

    const gchar *pwd = gtk_editable_get_text (GTK_EDITABLE (self->password_row));

    if (self->mode == PASSWORD_MODE_NEW || self->mode == PASSWORD_MODE_CHANGE)
    {
        const gchar *confirm = gtk_editable_get_text (GTK_EDITABLE (self->confirm_row));
        if (g_strcmp0 (pwd, confirm) != 0)
        {
            gtk_label_set_text (GTK_LABEL (self->error_label), _("Passwords do not match"));
            gtk_widget_set_visible (self->error_label, TRUE);
            return;
        }
    }

    if (self->mode == PASSWORD_MODE_CHANGE)
    {
        /* Return the current password first, then the new password via callback.
         * For CHANGE mode we return the new password; the caller uses current_password_row
         * separately before calling this. We pass the new password. */
    }

    /* Copy password into secure memory */
    gchar *secure_pwd = gcry_calloc_secure (strlen (pwd) + 1, 1);
    memcpy (secure_pwd, pwd, strlen (pwd) + 1);

    /* Clear password entry widgets */
    gtk_editable_set_text (GTK_EDITABLE (self->password_row), "");
    if (self->confirm_row != NULL)
        gtk_editable_set_text (GTK_EDITABLE (self->confirm_row), "");
    if (self->current_password_row != NULL)
        gtk_editable_set_text (GTK_EDITABLE (self->current_password_row), "");

    adw_dialog_close (ADW_DIALOG (self));

    if (self->callback != NULL)
        self->callback (secure_pwd, self->callback_data);

    gcry_free (secure_pwd);
}

static void
on_password_activate (AdwEntryRow    *row,
                      PasswordDialog *self)
{
    (void) row;

    if (gtk_widget_get_sensitive (self->unlock_button))
        on_unlock_clicked (GTK_BUTTON (self->unlock_button), self);
}

static void
on_dialog_map (GtkWidget      *widget,
               PasswordDialog *self)
{
    (void) widget;
    GtkWidget *focus_target = (self->mode == PASSWORD_MODE_CHANGE)
        ? self->current_password_row
        : self->password_row;
    gtk_widget_grab_focus (focus_target);
}

static void
password_dialog_dispose (GObject *object)
{
    G_OBJECT_CLASS (password_dialog_parent_class)->dispose (object);
}

static void
password_dialog_init (PasswordDialog *self)
{
    (void) self;
}

static void
password_dialog_class_init (PasswordDialogClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = password_dialog_dispose;
}

PasswordDialog *
password_dialog_new (PasswordDialogMode     mode,
                     PasswordDialogCallback callback,
                     gpointer               user_data)
{
    PasswordDialog *self = g_object_new (PASSWORD_TYPE_DIALOG,
                                         "title", "",
                                         "content-width", 360,
                                         "content-height", -1,
                                         NULL);

    self->mode = mode;
    self->callback = callback;
    self->callback_data = user_data;

    /* Build UI programmatically */
    GtkWidget *toolbar_view = adw_toolbar_view_new ();
    GtkWidget *header = adw_header_bar_new ();
    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);

    GtkWidget *clamp = adw_clamp_new ();
    gtk_widget_set_margin_start (clamp, 12);
    gtk_widget_set_margin_end (clamp, 12);
    gtk_widget_set_margin_top (clamp, 12);
    gtk_widget_set_margin_bottom (clamp, 12);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 18);
    adw_clamp_set_child (ADW_CLAMP (clamp), box);

    /* Status page (icon + title) */
    self->status_page = adw_status_page_new ();
    adw_status_page_set_icon_name (ADW_STATUS_PAGE (self->status_page), "dialog-password-symbolic");
    gtk_widget_add_css_class (self->status_page, "compact");

    switch (mode)
    {
        case PASSWORD_MODE_DECRYPT:
            adw_status_page_set_title (ADW_STATUS_PAGE (self->status_page), _("Unlock Database"));
            adw_status_page_set_description (ADW_STATUS_PAGE (self->status_page),
                                             _("Enter the password to decrypt your database"));
            break;
        case PASSWORD_MODE_NEW:
            adw_status_page_set_title (ADW_STATUS_PAGE (self->status_page), _("New Database"));
            adw_status_page_set_description (ADW_STATUS_PAGE (self->status_page),
                                             _("Choose a password for your new database"));
            break;
        case PASSWORD_MODE_CHANGE:
            adw_status_page_set_title (ADW_STATUS_PAGE (self->status_page), _("Change Password"));
            adw_status_page_set_description (ADW_STATUS_PAGE (self->status_page),
                                             _("Enter your current and new password"));
            break;
    }
    gtk_box_append (GTK_BOX (box), self->status_page);

    /* Preferences group with password fields */
    self->prefs_group = adw_preferences_group_new ();

    if (mode == PASSWORD_MODE_CHANGE)
    {
        self->current_password_row = adw_password_entry_row_new ();
        adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->current_password_row), _("Current Password"));
        g_signal_connect (self->current_password_row, "changed", G_CALLBACK (on_text_changed), self);
        adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->prefs_group), self->current_password_row);
    }

    self->password_row = adw_password_entry_row_new ();
    if (mode == PASSWORD_MODE_DECRYPT)
        adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->password_row), _("Password"));
    else
        adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->password_row), _("New Password"));
    g_signal_connect (self->password_row, "changed", G_CALLBACK (on_text_changed), self);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->prefs_group), self->password_row);

    if (mode == PASSWORD_MODE_NEW || mode == PASSWORD_MODE_CHANGE)
    {
        self->confirm_row = adw_password_entry_row_new ();
        adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->confirm_row), _("Confirm Password"));
        g_signal_connect (self->confirm_row, "changed", G_CALLBACK (on_text_changed), self);
        g_signal_connect (self->confirm_row, "entry-activated", G_CALLBACK (on_password_activate), self);
        adw_preferences_group_add (ADW_PREFERENCES_GROUP (self->prefs_group), self->confirm_row);
    }
    else
    {
        g_signal_connect (self->password_row, "entry-activated", G_CALLBACK (on_password_activate), self);
    }

    gtk_box_append (GTK_BOX (box), self->prefs_group);

    /* Error label */
    self->error_label = gtk_label_new (NULL);
    gtk_widget_add_css_class (self->error_label, "error");
    gtk_widget_set_visible (self->error_label, FALSE);
    gtk_box_append (GTK_BOX (box), self->error_label);

    /* Unlock button */
    self->unlock_button = gtk_button_new_with_label (
        mode == PASSWORD_MODE_DECRYPT ? _("Unlock") :
        mode == PASSWORD_MODE_NEW ? _("Create") : _("Change Password"));
    gtk_widget_add_css_class (self->unlock_button, "suggested-action");
    gtk_widget_add_css_class (self->unlock_button, "pill");
    gtk_widget_set_halign (self->unlock_button, GTK_ALIGN_CENTER);
    gtk_widget_set_sensitive (self->unlock_button, FALSE);
    g_signal_connect (self->unlock_button, "clicked", G_CALLBACK (on_unlock_clicked), self);
    gtk_box_append (GTK_BOX (box), self->unlock_button);

    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), clamp);
    adw_dialog_set_child (ADW_DIALOG (self), toolbar_view);

    g_signal_connect (self, "map", G_CALLBACK (on_dialog_map), self);

    return self;
}
