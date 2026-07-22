#define _DEFAULT_SOURCE
#include <glib/gi18n.h>
#include <gcrypt.h>
#include <string.h>
#include "password-dialog.h"

struct _PasswordDialog
{
    AdwDialog parent;

    PasswordDialogMode mode;
    PasswordDialogCallback callback;
    gpointer callback_data;

    GtkWidget *header;
    GtkWidget *status_page;
    GtkWidget *prefs_group;
    GtkWidget *current_password_row;
    GtkWidget *password_row;
    GtkWidget *confirm_row;
    GtkWidget *unlock_button;
    GtkWidget *error_label;

    gboolean locked_mode;
    gboolean disposed;
};

enum {
    SIGNAL_QUIT_REQUESTED,
    N_SIGNALS
};

static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (PasswordDialog, password_dialog, ADW_TYPE_DIALOG)

static void
wipe_editable (GtkWidget *widget)
{
    if (widget == NULL)
        return;
    const gchar *text = gtk_editable_get_text (GTK_EDITABLE (widget));
    if (text != NULL && text[0] != '\0')
        explicit_bzero ((gchar *) text, strlen (text));
    gtk_editable_set_text (GTK_EDITABLE (widget), "");
}

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
    const gchar *current_pwd = (self->current_password_row != NULL)
        ? gtk_editable_get_text (GTK_EDITABLE (self->current_password_row))
        : NULL;

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

    /* Copy password into secure memory */
    gsize pwd_len = strlen (pwd);
    gchar *secure_pwd = gcry_calloc_secure (pwd_len + 1, 1);
    if (secure_pwd == NULL) {
        gtk_label_set_text (GTK_LABEL (self->error_label),
                            _("Secure memory is exhausted"));
        gtk_widget_set_visible (self->error_label, TRUE);
        return;
    }
    memcpy (secure_pwd, pwd, pwd_len + 1);
    gchar *secure_current_pwd = NULL;
    gsize current_pwd_len = 0;
    if (self->mode == PASSWORD_MODE_CHANGE && current_pwd != NULL)
    {
        current_pwd_len = strlen (current_pwd);
        secure_current_pwd = gcry_calloc_secure (current_pwd_len + 1, 1);
        if (secure_current_pwd == NULL) {
            gcry_free (secure_pwd);
            gtk_label_set_text (GTK_LABEL (self->error_label),
                                _("Secure memory is exhausted"));
            gtk_widget_set_visible (self->error_label, TRUE);
            return;
        }
        memcpy (secure_current_pwd, current_pwd, current_pwd_len + 1);
    }

    /* GTK's AdwPasswordEntryRow keeps the plaintext in a regular GtkText
     * buffer that gtk_editable_set_text("") does NOT zero before
     * reallocating. Wipe the bytes in place first so a heap inspection of
     * the GUI process after unlock can't recover the password. pwd is a
     * const pointer into that buffer; the cast is intentional. Best-effort:
     * GTK is free to reallocate at any time, but this covers the common
     * case where the buffer is still the one we just read. */
    if (pwd_len > 0)
        explicit_bzero ((gchar *) pwd, pwd_len);
    if (current_pwd_len > 0)
        explicit_bzero ((gchar *) current_pwd, current_pwd_len);
    if (self->confirm_row != NULL)
    {
        const gchar *confirm_pwd = gtk_editable_get_text (GTK_EDITABLE (self->confirm_row));
        if (confirm_pwd != NULL && confirm_pwd[0] != '\0')
            explicit_bzero ((gchar *) confirm_pwd, strlen (confirm_pwd));
    }

    gboolean accepted = TRUE;
    g_autofree gchar *error_message = NULL;
    if (self->callback != NULL)
        accepted = self->callback (secure_current_pwd, secure_pwd, &error_message, self->callback_data);

    if (!accepted)
    {
        gtk_label_set_text (GTK_LABEL (self->error_label),
                            error_message != NULL ? error_message : _("Password was rejected"));
        gtk_widget_set_visible (self->error_label, TRUE);
        gtk_editable_set_text (GTK_EDITABLE (self->password_row), "");
        if (self->confirm_row != NULL)
            gtk_editable_set_text (GTK_EDITABLE (self->confirm_row), "");
        if (self->current_password_row != NULL)
            gtk_editable_set_text (GTK_EDITABLE (self->current_password_row), "");
        if (secure_current_pwd != NULL)
            gcry_free (secure_current_pwd);
        gcry_free (secure_pwd);
        update_unlock_sensitivity (self);
        return;
    }

    /* Clear password entry widgets */
    gtk_editable_set_text (GTK_EDITABLE (self->password_row), "");
    if (self->confirm_row != NULL)
        gtk_editable_set_text (GTK_EDITABLE (self->confirm_row), "");
    if (self->current_password_row != NULL)
        gtk_editable_set_text (GTK_EDITABLE (self->current_password_row), "");

    /* Order matters: lock-state callers attach a "closed" handler that drops
     * the app to the locked page unless an unlock is in flight. Run the
     * callback first so it marks the unlock in-progress before the close fires
     * that handler, otherwise a successful unlock would race with a stray drop
     * to the locked page. */
    adw_dialog_force_close (ADW_DIALOG (self));

    if (secure_current_pwd != NULL)
        gcry_free (secure_current_pwd);
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
    PasswordDialog *self = PASSWORD_DIALOG (object);
    if (!self->disposed) {
        self->disposed = TRUE;
        wipe_editable (self->current_password_row);
        wipe_editable (self->password_row);
        wipe_editable (self->confirm_row);
    }
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

    signals[SIGNAL_QUIT_REQUESTED] =
        g_signal_new ("quit-requested",
                      G_TYPE_FROM_CLASS (klass),
                      G_SIGNAL_RUN_LAST,
                      0, NULL, NULL, NULL,
                      G_TYPE_NONE, 0);
}

static void
on_quit_button_clicked (GtkButton      *button,
                        PasswordDialog *self)
{
    (void) button;
    g_signal_emit (self, signals[SIGNAL_QUIT_REQUESTED], 0);
}

void
password_dialog_set_locked_mode (PasswordDialog *self)
{
    g_return_if_fail (PASSWORD_IS_DIALOG (self));

    if (self->locked_mode)
        return;
    self->locked_mode = TRUE;

    /* The dialog stays dismissable (can_close defaults to TRUE): Escape, the
     * dialog X, or the parent window's close button fire the normal "closed"
     * signal, which lock-state callers use to drop to the locked page instead
     * of quitting (#467). The Quit button remains the explicit way out. */
    GtkWidget *quit_button = gtk_button_new_with_label (_("Quit"));
    gtk_widget_add_css_class (quit_button, "flat");
    g_signal_connect (quit_button, "clicked", G_CALLBACK (on_quit_button_clicked), self);
    adw_header_bar_pack_start (ADW_HEADER_BAR (self->header), quit_button);
}

PasswordDialog *
password_dialog_new (PasswordDialogMode     mode,
                     PasswordDialogCallback callback,
                     gpointer               user_data)
{
    PasswordDialog *self = g_object_new (PASSWORD_TYPE_DIALOG,
                                         "title", "",
                                         "content-width", 440,
                                         "content-height", 480,
                                         NULL);

    self->mode = mode;
    self->callback = callback;
    self->callback_data = user_data;

    /* Build UI programmatically */
    GtkWidget *toolbar_view = adw_toolbar_view_new ();
    self->header = adw_header_bar_new ();
    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), self->header);

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

    GtkWidget *scrolled = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scrolled), TRUE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), clamp);
    gtk_widget_set_vexpand (scrolled, TRUE);
    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), scrolled);
    adw_dialog_set_child (ADW_DIALOG (self), toolbar_view);

    g_signal_connect (self, "map", G_CALLBACK (on_dialog_map), self);

    return self;
}
