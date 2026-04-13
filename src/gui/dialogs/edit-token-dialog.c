#include <glib/gi18n.h>
#include "edit-token-dialog.h"
#include "common.h"
#include "db-common.h"

struct _EditTokenDialog
{
    AdwDialog parent;

    json_t *token_obj;
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

static void
on_save_clicked (GtkButton       *button,
                 EditTokenDialog *self)
{
    (void) button;

    const gchar *label_text = gtk_editable_get_text (GTK_EDITABLE (self->label_row));
    const gchar *issuer = gtk_editable_get_text (GTK_EDITABLE (self->issuer_row));

    const gchar *group_text = gtk_editable_get_text (GTK_EDITABLE (self->group_row));

    json_object_set (self->token_obj, "label", json_string (label_text));
    json_object_set (self->token_obj, "issuer", json_string (issuer));

    if (group_text != NULL && group_text[0] != '\0')
        json_object_set (self->token_obj, "group", json_string (group_text));
    else
        json_object_del (self->token_obj, "group");

    GError *err = NULL;
    update_db (self->db_data, &err);
    if (err != NULL)
    {
        gtk_label_set_text (GTK_LABEL (self->error_label), err->message);
        gtk_widget_set_visible (self->error_label, TRUE);
        g_clear_error (&err);
        return;
    }

    /* Recompute hashes */
    g_slist_free_full (self->db_data->objects_hash, g_free);
    self->db_data->objects_hash = NULL;

    gsize index;
    json_t *obj;
    json_array_foreach (self->db_data->in_memory_json_data, index, obj)
    {
        guint32 hash = json_object_get_hash (obj);
        self->db_data->objects_hash = g_slist_append (self->db_data->objects_hash,
                                                       g_memdup2 (&hash, sizeof (guint32)));
    }

    adw_dialog_close (ADW_DIALOG (self));

    if (self->callback != NULL)
        self->callback (self->callback_data);
}

static void
edit_token_dialog_dispose (GObject *object)
{
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
                                          "content-width", 360,
                                          "content-height", -1,
                                          NULL);

    self->token_obj = token_obj;
    self->token_index = token_index;
    self->db_data = db_data;
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

    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), clamp);
    adw_dialog_set_child (ADW_DIALOG (self), toolbar_view);

    return self;
}
