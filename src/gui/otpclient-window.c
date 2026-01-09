#include <glib/gi18n.h>
#include <adwaita.h>
#include "otpclient-application.h"
#include "otpclient-window.h"

struct _OTPClientWindow
{
    AdwApplicationWindow parent;

    GSettings *settings;
    GtkWidget *split_view;
    GtkWidget *back_button;
    GtkWidget *add_button;
    GtkWidget *reorder_button;
    GtkWidget *search_bar;
    GtkWidget *search_entry;
    GtkWidget *lock_button;
    GtkWidget *settings_button;
    GtkWidget *database_list;
    GtkWidget *otp_list;
    GListStore *otp_store;
    GtkSingleSelection *otp_selection;
};

G_DEFINE_FINAL_TYPE (OTPClientWindow, otpclient_window, ADW_TYPE_APPLICATION_WINDOW)

typedef struct
{
    GObject parent_instance;
    gchar *account;
    gchar *issuer;
    gchar *otp_value;
} OTPEntry;

typedef struct
{
    GObjectClass parent_class;
} OTPEntryClass;

G_DEFINE_TYPE (OTPEntry, otp_entry, G_TYPE_OBJECT)

static void
otp_entry_finalize (GObject *object)
{
    OTPEntry *entry = (OTPEntry *) object;

    g_clear_pointer (&entry->account, g_free);
    g_clear_pointer (&entry->issuer, g_free);
    g_clear_pointer (&entry->otp_value, g_free);

    G_OBJECT_CLASS (otp_entry_parent_class)->finalize (object);
}

static void
otp_entry_class_init (OTPEntryClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = otp_entry_finalize;
}

static void
otp_entry_init (OTPEntry *entry)
{
    (void) entry;
}

static OTPEntry *
otp_entry_new (const gchar *account,
               const gchar *issuer,
               const gchar *otp_value)
{
    OTPEntry *entry = g_object_new (otp_entry_get_type (), NULL);

    entry->account = g_strdup (account);
    entry->issuer = g_strdup (issuer);
    entry->otp_value = g_strdup (otp_value);

    return entry;
}

typedef enum
{
    OTP_COLUMN_ACCOUNT,
    OTP_COLUMN_ISSUER,
    OTP_COLUMN_VALUE
} OTPColumn;

typedef struct
{
    GtkWidget *label;
    guint timeout_id;
    guint remaining;
} ValidityWidgets;

static void
validity_widgets_free (ValidityWidgets *widgets)
{
    if (widgets->timeout_id != 0)
        g_source_remove (widgets->timeout_id);

    g_free (widgets);
}

static void
validity_update_label (ValidityWidgets *widgets)
{
    gchar label_text[8];

    // Add safety check
    if (widgets == NULL || widgets->label == NULL || !GTK_IS_LABEL (widgets->label))
        return;

    g_snprintf (label_text, sizeof label_text, "%us", widgets->remaining);
    gtk_label_set_text (GTK_LABEL (widgets->label), label_text);
}

static gboolean
validity_tick (gpointer data)
{
    GtkListItem *list_item = GTK_LIST_ITEM (data);
    ValidityWidgets *widgets;

    // Check if list_item is still valid and not being destroyed
    if (!GTK_IS_LIST_ITEM (list_item))
        return G_SOURCE_REMOVE;

    // Check if the list item's parent widget is still valid
    GtkWidget *child = gtk_list_item_get_child (list_item);
    if (child == NULL || !GTK_IS_WIDGET (child))
        return G_SOURCE_REMOVE;

    widgets = g_object_get_data (G_OBJECT (list_item), "validity-widgets");

    if (widgets == NULL)
        return G_SOURCE_REMOVE;

    if (!gtk_list_item_get_selected (list_item))
    {
        widgets->timeout_id = 0;
        return G_SOURCE_REMOVE;
    }

    if (widgets->remaining == 0)
    {
        widgets->timeout_id = 0;
        return G_SOURCE_REMOVE;
    }

    widgets->remaining--;
    validity_update_label (widgets);

    if (widgets->remaining == 0)
    {
        widgets->timeout_id = 0;
        return G_SOURCE_REMOVE;
    }

    return G_SOURCE_CONTINUE;
}

static void
validity_selected_changed (GtkListItem *list_item,
                           GParamSpec  *pspec,
                           gpointer     user_data)
{
    (void) pspec;
    (void) user_data;

    ValidityWidgets *widgets;
    gboolean selected;

    if (!GTK_IS_LIST_ITEM (list_item))
        return;

    widgets = g_object_get_data (G_OBJECT (list_item), "validity-widgets");

    if (widgets == NULL)
        return;

    selected = gtk_list_item_get_selected (list_item);

    if (widgets->timeout_id != 0)
    {
        g_source_remove (widgets->timeout_id);
        widgets->timeout_id = 0;
    }

    if (selected)
    {
        widgets->remaining = 30;
        validity_update_label (widgets);
        gtk_widget_set_visible (widgets->label, TRUE);
        widgets->timeout_id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT,
                                                          1,
                                                          validity_tick,
                                                          g_object_ref (list_item),
                                                          g_object_unref);
    }
    else
    {
        gtk_widget_set_visible (widgets->label, FALSE);
    }
}

static void
validity_list_item_unbind (GtkSignalListItemFactory *factory,
                           GtkListItem              *list_item,
                           gpointer                  user_data)
{
    (void) factory;
    (void) user_data;

    ValidityWidgets *widgets = g_object_get_data (G_OBJECT (list_item), "validity-widgets");

    if (widgets == NULL)
        return;

    if (widgets->timeout_id != 0)
    {
        g_source_remove (widgets->timeout_id);
        widgets->timeout_id = 0;
    }

    gtk_widget_set_visible (widgets->label, FALSE);
}

static void
otp_text_column_setup (GtkSignalListItemFactory *factory,
                       GtkListItem              *list_item,
                       gpointer                  user_data)
{
    (void) factory;
    (void) user_data;

    GtkWidget *label = gtk_label_new (NULL);

    gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
    gtk_widget_set_hexpand (label, TRUE);
    gtk_list_item_set_child (list_item, label);
}

static void
otp_text_column_bind (GtkSignalListItemFactory *factory,
                      GtkListItem              *list_item,
                      gpointer                  user_data)
{
    (void) factory;

    OTPEntry *entry = gtk_list_item_get_item (list_item);
    GtkWidget *label = gtk_list_item_get_child (list_item);
    OTPColumn column = GPOINTER_TO_INT (user_data);
    const gchar *text = "";

    if (entry == NULL || label == NULL)
        return;

    switch (column)
    {
        case OTP_COLUMN_ACCOUNT:
            text = entry->account ? entry->account : "";
            break;
        case OTP_COLUMN_ISSUER:
            text = entry->issuer ? entry->issuer : "";
            break;
        case OTP_COLUMN_VALUE:
            text = entry->otp_value ? entry->otp_value : "";
            break;
        default:
            break;
    }

    gtk_label_set_text (GTK_LABEL (label), text);
}

static void
otp_validity_column_setup (GtkSignalListItemFactory *factory,
                           GtkListItem              *list_item,
                           gpointer                  user_data)
{
    (void) factory;
    (void) user_data;

    GtkWidget *label = gtk_label_new (NULL);
    ValidityWidgets *widgets = g_new0 (ValidityWidgets, 1);

    gtk_widget_set_visible (label, FALSE);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
    gtk_list_item_set_child (list_item, label);

    widgets->label = label;
    widgets->remaining = 30;
    g_object_set_data_full (G_OBJECT (list_item), "validity-widgets", widgets, (GDestroyNotify) validity_widgets_free);

    g_signal_connect (list_item, "notify::selected", G_CALLBACK (validity_selected_changed), NULL);
}

static void
otp_validity_column_bind (GtkSignalListItemFactory *factory,
                          GtkListItem              *list_item,
                          gpointer                  user_data)
{
    (void) factory;
    (void) user_data;

    validity_selected_changed (list_item, NULL, NULL);
}

static void
add_text_column (GtkColumnView *view,
                 const gchar   *title,
                 OTPColumn      column)
{
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
    GtkColumnViewColumn *view_column = gtk_column_view_column_new (title, factory);

    g_signal_connect (factory, "setup", G_CALLBACK (otp_text_column_setup), NULL);
    g_signal_connect (factory, "bind", G_CALLBACK (otp_text_column_bind), GINT_TO_POINTER (column));

    // Set column properties
    gtk_column_view_column_set_expand (view_column, TRUE);
    gtk_column_view_column_set_resizable (view_column, TRUE);

    gtk_column_view_append_column (view, view_column);
}

static void
add_validity_column (GtkColumnView *view)
{
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
    GtkColumnViewColumn *view_column = gtk_column_view_column_new (_("Validity"), factory);

    g_signal_connect (factory, "setup", G_CALLBACK (otp_validity_column_setup), NULL);
    g_signal_connect (factory, "bind", G_CALLBACK (otp_validity_column_bind), NULL);
    g_signal_connect (factory, "unbind", G_CALLBACK (validity_list_item_unbind), NULL);

    // Set fixed width for validity column
    gtk_column_view_column_set_fixed_width (view_column, 80);
    gtk_column_view_column_set_resizable (view_column, FALSE);

    gtk_column_view_append_column (view, view_column);
}

static void
setup_otp_view (OTPClientWindow *self)
{
    self->otp_store = G_LIST_STORE (g_object_ref_sink (g_list_store_new (otp_entry_get_type ())));
    self->otp_selection = gtk_single_selection_new (G_LIST_MODEL (self->otp_store));

    gtk_single_selection_set_autoselect (self->otp_selection, FALSE);
    gtk_single_selection_set_can_unselect (self->otp_selection, TRUE);

    // Add columns BEFORE setting the model
    add_text_column (GTK_COLUMN_VIEW (self->otp_list), _("Account"), OTP_COLUMN_ACCOUNT);
    add_text_column (GTK_COLUMN_VIEW (self->otp_list), _("Issuer"), OTP_COLUMN_ISSUER);
    add_text_column (GTK_COLUMN_VIEW (self->otp_list), _("OTP Value"), OTP_COLUMN_VALUE);
    add_validity_column (GTK_COLUMN_VIEW (self->otp_list));

    // Set the model AFTER columns are added
    gtk_column_view_set_model (GTK_COLUMN_VIEW (self->otp_list), GTK_SELECTION_MODEL (self->otp_selection));
}

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
populate_otp_list (OTPClientWindow *self,
                   guint            database_index)
{
    static const gchar *otp_entries_personal[][3] = {
        { "user@example.com", "GitHub", "741 283" },
        { "personal@vault.com", "1Password", "551 902" },
        { "user@example.com", "Dropbox", "893 104" },
        { NULL, NULL, NULL }
    };
    static const gchar *otp_entries_work[][3] = {
        { "team@company.com", "Google Workspace", "334 228" },
        { "corp-admin", "Okta", "190 660" },
        { "corp", "VPN", "554 932" },
        { NULL, NULL, NULL }
    };
    const gchar * const (*entries)[3] = otp_entries_personal;

    if (database_index == 1)
        entries = otp_entries_work;

    g_list_store_remove_all (self->otp_store);

    for (guint i = 0; entries[i][0] != NULL; i++)
    {
        OTPEntry *entry = otp_entry_new (entries[i][0], entries[i][1], entries[i][2]);

        g_list_store_append (self->otp_store, entry);
        g_object_unref (entry);
    }
}

static void
database_row_selected (GtkListBox      *box,
                       GtkListBoxRow   *row,
                       OTPClientWindow *self)
{
    (void) box;

    if (row == NULL)
        return;

    populate_otp_list (self, gtk_list_box_row_get_index (row));
    adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (self->split_view), TRUE);
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
back_button_clicked (GtkButton       *button,
                     OTPClientWindow *self)
{
    (void) button;

    gtk_single_selection_set_selected (self->otp_selection, GTK_INVALID_LIST_POSITION);
    adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (self->split_view), FALSE);
}

static void
update_back_button (OTPClientWindow *self)
{
    gboolean collapsed = adw_navigation_split_view_get_collapsed (ADW_NAVIGATION_SPLIT_VIEW (self->split_view));
    gboolean show_content = adw_navigation_split_view_get_show_content (ADW_NAVIGATION_SPLIT_VIEW (self->split_view));

    gtk_widget_set_visible (self->back_button, collapsed && show_content);
}

static void
split_view_state_changed (AdwNavigationSplitView *view,
                          GParamSpec             *pspec,
                          OTPClientWindow        *self)
{
    (void) view;
    (void) pspec;

    if (!adw_navigation_split_view_get_show_content (ADW_NAVIGATION_SPLIT_VIEW (self->split_view)))
        gtk_single_selection_set_selected (self->otp_selection, GTK_INVALID_LIST_POSITION);

    if (!adw_navigation_split_view_get_collapsed (ADW_NAVIGATION_SPLIT_VIEW (self->split_view)))
        adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (self->split_view), TRUE);

    update_back_button (self);
}

static void
otpclient_window_dispose (GObject *object)
{
    OTPClientWindow *win = OTPCLIENT_WINDOW(object);

    // 1. Disconnect the view from the selection model
    if (win->otp_list && GTK_IS_COLUMN_VIEW (win->otp_list))
        gtk_column_view_set_model (GTK_COLUMN_VIEW (win->otp_list), NULL);

    // 2. Disconnect the selection model from the store
    if (win->otp_selection)
        gtk_single_selection_set_model (win->otp_selection, NULL);

    // 3. Clear the selection object itself
    g_clear_object (&win->otp_selection);

    // 4. Clear the otp_store
    g_clear_object (&win->otp_store);

    // 5. Clear other non-widget objects
    g_clear_object (&win->settings);

    // 6. Chain up
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

    setup_otp_view (self);
    setup_lists (self);
    adw_navigation_split_view_set_show_content (ADW_NAVIGATION_SPLIT_VIEW (self->split_view),
                                                !adw_navigation_split_view_get_collapsed (ADW_NAVIGATION_SPLIT_VIEW (self->split_view)));
    update_back_button (self);

    g_signal_connect (self->split_view, "notify::collapsed", G_CALLBACK (split_view_state_changed), self);
    g_signal_connect (self->split_view, "notify::show-content", G_CALLBACK (split_view_state_changed), self);
    g_signal_connect (self->back_button, "clicked", G_CALLBACK (back_button_clicked), self);
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
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, back_button);
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
