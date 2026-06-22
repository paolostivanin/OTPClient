#include <glib/gi18n.h>
#include "edit-token-dialog.h"
#include "common.h"
#include "db-common.h"
#include "gquarks.h"
#include "otp-validation.h"
#include "../otpclient-application.h"

struct _EditTokenDialog
{
    AdwDialog parent;

    json_t *token_obj;
    json_t *original_token;
    guint token_index;
    DatabaseData *db_data;
    EditTokenCallback callback;
    gpointer callback_data;

    GtkWidget *label_row;
    GtkWidget *issuer_row;
    GtkWidget *group_row;
    GtkWidget *save_button;
    GtkWidget *error_label;
};

G_DEFINE_FINAL_TYPE (EditTokenDialog, edit_token_dialog, ADW_TYPE_DIALOG)

typedef struct {
    guint token_index;
    json_t *original_token;
    gchar *label;
    gchar *issuer;
    gchar *group;
} EditTokenMutation;

static gboolean
edit_token_mutation (json_t   *candidate,
                     gpointer  user_data,
                     GError  **err)
{
    EditTokenMutation *mutation = user_data;
    json_t *token_obj = json_array_get (candidate, mutation->token_index);
    if (token_obj == NULL || !json_equal (token_obj, mutation->original_token)) {
        g_set_error (err, generic_error_gquark (), GENERIC_ERRCODE,
                     "%s", _("Token changed while the edit dialog was open."));
        return FALSE;
    }

    json_object_set_new (token_obj, "label", json_string (mutation->label));
    json_object_set_new (token_obj, "issuer", json_string (mutation->issuer));

    if (mutation->group != NULL && mutation->group[0] != '\0')
        json_object_set_new (token_obj, "group", json_string (mutation->group));
    else
        json_object_del (token_obj, "group");

    return otp_validate_token_object (token_obj, mutation->token_index, err);
}

static void
on_save_clicked (GtkButton       *button,
                 EditTokenDialog *self)
{
    (void) button;

    GApplication *default_app = g_application_get_default ();
    OTPClientApplication *app = OTPCLIENT_IS_APPLICATION (default_app)
        ? OTPCLIENT_APPLICATION (default_app)
        : NULL;
    if (app == NULL || otpclient_application_get_app_locked (app) ||
        otpclient_application_get_db_data (app) != self->db_data)
    {
        gtk_label_set_text (GTK_LABEL (self->error_label),
                            _("The active database changed. Reopen this token before saving."));
        gtk_widget_set_visible (self->error_label, TRUE);
        return;
    }

    const gchar *label_text = gtk_editable_get_text (GTK_EDITABLE (self->label_row));
    const gchar *issuer = gtk_editable_get_text (GTK_EDITABLE (self->issuer_row));
    const gchar *group_text = gtk_editable_get_text (GTK_EDITABLE (self->group_row));

    GError *err = NULL;
    EditTokenMutation mutation = {
        .token_index = self->token_index,
        .original_token = self->original_token,
        .label = (gchar *) label_text,
        .issuer = (gchar *) issuer,
        .group = (gchar *) group_text,
    };
    db_transaction (self->db_data, edit_token_mutation, &mutation, &err);
    if (err != NULL)
    {
        gtk_label_set_text (GTK_LABEL (self->error_label), err->message);
        gtk_widget_set_visible (self->error_label, TRUE);
        g_clear_error (&err);
        return;
    }

    adw_dialog_close (ADW_DIALOG (self));

    if (self->callback != NULL)
        self->callback (self->callback_data);
}

static void
edit_token_dialog_dispose (GObject *object)
{
    EditTokenDialog *self = EDIT_TOKEN_DIALOG (object);
    g_clear_pointer (&self->original_token, json_decref);
    g_clear_pointer (&self->db_data, database_data_free);

    G_OBJECT_CLASS (edit_token_dialog_parent_class)->dispose (object);
}

static void
edit_token_dialog_init (EditTokenDialog *self)
{
    (void) self;
}

static void
edit_token_dialog_class_init (EditTokenDialogClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    object_class->dispose = edit_token_dialog_dispose;
}

EditTokenDialog *
edit_token_dialog_new (json_t            *token_obj,
                       guint              token_index,
                       DatabaseData      *db_data,
                       EditTokenCallback  callback,
                       gpointer           user_data)
{
    EditTokenDialog *self = g_object_new (EDIT_TOKEN_TYPE_DIALOG,
                                          "title", _("Edit Token"),
                                          "content-width", 440,
                                          "content-height", 500,
                                          NULL);

    self->token_obj = token_obj;
    self->original_token = json_deep_copy (token_obj);
    self->token_index = token_index;
    self->db_data = database_data_ref (db_data);
    self->callback = callback;
    self->callback_data = user_data;

    /* Build UI */
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

    GtkWidget *group = adw_preferences_group_new ();

    /* Label */
    self->label_row = adw_entry_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->label_row), _("Label"));
    const gchar *label = json_string_value (json_object_get (token_obj, "label"));
    if (label != NULL)
        gtk_editable_set_text (GTK_EDITABLE (self->label_row), label);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), self->label_row);

    /* Issuer */
    self->issuer_row = adw_entry_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->issuer_row), _("Issuer"));
    const gchar *issuer = json_string_value (json_object_get (token_obj, "issuer"));
    if (issuer != NULL)
        gtk_editable_set_text (GTK_EDITABLE (self->issuer_row), issuer);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), self->issuer_row);

    /* Group */
    self->group_row = adw_entry_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->group_row), _("Group"));
    const gchar *grp = json_string_value (json_object_get (token_obj, "group"));
    if (grp != NULL)
        gtk_editable_set_text (GTK_EDITABLE (self->group_row), grp);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), self->group_row);

    gtk_box_append (GTK_BOX (box), group);

    /* Read-only info */
    GtkWidget *info_group = adw_preferences_group_new ();
    adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (info_group), _("Token Info"));

    const gchar *type = json_string_value (json_object_get (token_obj, "type"));
    const gchar *algo = json_string_value (json_object_get (token_obj, "algo"));
    json_int_t digits = json_integer_value (json_object_get (token_obj, "digits"));

    GtkWidget *type_row = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (type_row), _("Type"));
    adw_action_row_set_subtitle (ADW_ACTION_ROW (type_row), type ? type : "TOTP");
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (info_group), type_row);

    GtkWidget *algo_row = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (algo_row), _("Algorithm"));
    adw_action_row_set_subtitle (ADW_ACTION_ROW (algo_row), algo ? algo : "SHA1");
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (info_group), algo_row);

    g_autofree gchar *digits_str = g_strdup_printf ("%ld", (long) digits);
    GtkWidget *digits_row = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (digits_row), _("Digits"));
    adw_action_row_set_subtitle (ADW_ACTION_ROW (digits_row), digits_str);
    adw_preferences_group_add (ADW_PREFERENCES_GROUP (info_group), digits_row);

    gtk_box_append (GTK_BOX (box), info_group);

    /* Error label */
    self->error_label = gtk_label_new (NULL);
    gtk_widget_add_css_class (self->error_label, "error");
    gtk_widget_set_visible (self->error_label, FALSE);
    gtk_box_append (GTK_BOX (box), self->error_label);

    /* Save button */
    self->save_button = gtk_button_new_with_label (_("Save"));
    gtk_widget_add_css_class (self->save_button, "suggested-action");
    gtk_widget_add_css_class (self->save_button, "pill");
    gtk_widget_set_halign (self->save_button, GTK_ALIGN_CENTER);
    g_signal_connect (self->save_button, "clicked", G_CALLBACK (on_save_clicked), self);
    gtk_box_append (GTK_BOX (box), self->save_button);

    GtkWidget *scrolled = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled),
                                    GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scrolled), TRUE);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), clamp);
    gtk_widget_set_vexpand (scrolled, TRUE);
    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), scrolled);
    adw_dialog_set_child (ADW_DIALOG (self), toolbar_view);

    return self;
}
