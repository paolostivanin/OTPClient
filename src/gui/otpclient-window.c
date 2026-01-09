#include <glib/gi18n.h>
#include <adwaita.h>
#include "otpclient-application.h"
#include "otpclient-window.h"

struct _OTPClientWindow
{
    AdwApplicationWindow parent;

    GSettings *settings;
    GtkWidget *split_view;
    GtkWidget *add_button;
    GtkWidget *reorder_button;
    GtkWidget *search_bar;
    GtkWidget *search_entry;
    GtkWidget *lock_button;
    GtkWidget *settings_button;
    GtkWidget *database_list;
    GtkWidget *otp_list;
};

G_DEFINE_FINAL_TYPE (OTPClientWindow, otpclient_window, ADW_TYPE_APPLICATION_WINDOW)

static void
search_func (OTPClientWindow *self,
             const gchar     *action_name,
             GVariant        *parameter)
{
    (void) action_name;
    (void) parameter;

    gboolean search_enabled = gtk_search_bar_get_search_mode (GTK_SEARCH_BAR(self->search_bar));
    gtk_search_bar_set_search_mode (GTK_SEARCH_BAR(self->search_bar), !search_enabled);
    if (!search_enabled)
        gtk_widget_grab_focus (self->search_entry);
}

static void
search_text_changed (GtkEntry        *entry,
                     OTPClientWindow *win)
{
    (void) win;

    const char *text = gtk_editable_get_text (GTK_EDITABLE(entry));
    if (text[0] == '\0')
        return;

    g_print ("%s\n", text);
}

static void
add_list_row (GtkListBox   *list,
              const gchar  *title,
              const gchar  *subtitle)
{
    AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());

    g_object_set (row, "title", title, NULL);
    if (subtitle != NULL)
        g_object_set (row, "subtitle", subtitle, NULL);

    gtk_list_box_append (list, GTK_WIDGET (row));
}

static void
clear_list_box (GtkListBox *list)
{
    GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (list));

    while (child != NULL)
    {
        GtkWidget *next = gtk_widget_get_next_sibling (child);
        gtk_list_box_remove (list, child);
        child = next;
    }
}

static void
populate_otp_list (OTPClientWindow *self,
                   guint            database_index)
{
    static const gchar *otp_entries_personal[][2] = {
        { "GitHub", "user@example.com" },
        { "1Password", "personal vault" },
        { "Dropbox", "user@example.com" },
        { NULL, NULL }
    };
    static const gchar *otp_entries_work[][2] = {
        { "Google Workspace", "team@company.com" },
        { "Okta", "corp-admin" },
        { "VPN", "corp" },
        { NULL, NULL }
    };
    const gchar * const (*entries)[2] = otp_entries_personal;

    if (database_index == 1)
        entries = otp_entries_work;

    clear_list_box (GTK_LIST_BOX (self->otp_list));

    for (guint i = 0; entries[i][0] != NULL; i++)
        add_list_row (GTK_LIST_BOX (self->otp_list), entries[i][0], entries[i][1]);
}

static void
database_row_selected (GtkListBox      *box,
                       GtkListBoxRow   *row,
                       OTPClientWindow *self)
{
    if (row == NULL)
        return;

    populate_otp_list (self, gtk_list_box_row_get_index (row));
}

static void
setup_lists (OTPClientWindow *self)
{
    static const gchar *databases[] = {
        "Personal Vault",
        "Work Accounts",
        NULL
    };

    for (guint i = 0; databases[i] != NULL; i++)
        add_list_row (GTK_LIST_BOX (self->database_list), databases[i], NULL);

    gtk_list_box_select_row (GTK_LIST_BOX (self->database_list),
                             gtk_list_box_get_row_at_index (GTK_LIST_BOX (self->database_list), 0));

    g_signal_connect (self->database_list, "row-selected", G_CALLBACK (database_row_selected), self);
    populate_otp_list (self, 0);
}

static void
otpclient_window_dispose (GObject *object)
{
    OTPClientWindow *win = OTPCLIENT_WINDOW(object);
    g_clear_object (&win->settings);

    gtk_widget_dispose_template (GTK_WIDGET (object), OTPCLIENT_TYPE_WINDOW);
    G_OBJECT_CLASS (otpclient_window_parent_class)->dispose (object);
}

static void
otpclient_window_init (OTPClientWindow *self)
{
    GSettingsSchemaSource *schema_source = NULL;
    g_autoptr (GSettingsSchema) schema = NULL;

    gtk_widget_init_template (GTK_WIDGET(self));

    schema_source = g_settings_schema_source_get_default ();
    if (schema_source != NULL)
        schema = g_settings_schema_source_lookup (schema_source, "com.github.paolostivanin.OTPClient", TRUE);

    if (schema != NULL)
        self->settings = g_settings_new ("com.github.paolostivanin.OTPClient");
    else
        g_warning ("Settings schema 'com.github.paolostivanin.OTPClient' is not installed.");

    setup_lists (self);
}

static void
otpclient_window_class_init (OTPClientWindowClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = otpclient_window_dispose;

    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    gtk_widget_class_set_template_from_resource (widget_class, "/com/github/paolostivanin/OTPClient/ui/window.ui");
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, add_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, reorder_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, search_bar);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, search_entry);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, lock_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, settings_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, split_view);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, database_list);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, otp_list);

    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_q, GDK_CONTROL_MASK, "window.close", NULL);

    gtk_widget_class_install_action (widget_class, "window.search", NULL, (GtkWidgetActionActivateFunc)search_func);
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_f, GDK_CONTROL_MASK, "window.search", NULL);

    gtk_widget_class_bind_template_callback (klass, search_text_changed);
}

GtkWidget *
otpclient_window_new (OTPClientApplication *application)
{
    return g_object_new (OTPCLIENT_TYPE_WINDOW, "application", application, NULL);
}
