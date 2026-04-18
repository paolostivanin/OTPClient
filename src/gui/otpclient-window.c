#include <glib/gi18n.h>
#include <adwaita.h>
#include "otpclient-application.h"
#include "otpclient-window.h"
#include "otp-entry.h"
#include "database-sidebar.h"
#include "db-common.h"
#include "qrcode-parser.h"
#include "webcam-scanner.h"
#include "parse-uri.h"
#include "dialogs/manual-add-dialog.h"
#include "dialogs/import-dialog.h"
#include "dialogs/export-dialog.h"
#include "dialogs/edit-token-dialog.h"
#include "dialogs/qr-display-dialog.h"
#include "dialogs/settings-dialog.h"
#include "dialogs/password-dialog.h"
#include "gui-misc.h"
#include "lock-app.h"
#include "secret-schema.h"
#include "gsettings-common.h"
#include "common.h"

struct _OTPClientWindow
{
    AdwApplicationWindow parent;

    GSettings *settings;
    GtkWidget *toast_overlay;
    GtkWidget *split_view;
    GtkWidget *sidebar_toggle_button;
    GtkWidget *add_button;
    GtkWidget *search_bar;
    GtkWidget *search_entry;
    GtkWidget *lock_button;
    GtkWidget *settings_button;
    GtkWidget *database_list;
    GtkWidget *new_db_button;
    GtkWidget *open_db_button;
    GtkWidget *otp_list;
    GtkWidget *content_stack;
    GListStore *otp_store;
    GtkFilterListModel *filter_model;
    GtkCustomFilter *search_filter;
    GtkSortListModel *sort_model;
    GtkSingleSelection *otp_selection;

    GListStore *db_store;

    guint otp_refresh_timer_id;

    /* Drag-and-drop state */
    GtkWidget *dnd_highlight_row;
    GtkCssProvider *dnd_css_provider;

    /* Group filter */
    GtkWidget *group_dropdown;
    GtkStringList *group_list_model;
    gchar *active_group_filter;  /* NULL = "All", "" = "Ungrouped", non-empty = group name */
    gboolean syncing_group_filter;

    /* Cross-database search */
    GListStore *cross_db_store;
    GtkFlattenListModel *flatten_model;
    gboolean cross_db_loaded;
    gboolean cross_db_loading;

    /* Clipboard auto-clear */
    guint clipboard_clear_timer_id;

    /* Suppress clipboard copy + notification for programmatic selection changes */
    gboolean suppress_selection_action;

    /* Undo delete */
    json_t *deleted_token;
    guint   deleted_token_pos;
};

G_DEFINE_FINAL_TYPE (OTPClientWindow, otpclient_window, ADW_TYPE_APPLICATION_WINDOW)

typedef enum
{
    OTP_COLUMN_ACCOUNT,
    OTP_COLUMN_ISSUER,
    OTP_COLUMN_VALUE
} OTPColumn;

typedef struct
{
    GtkWidget *label;
    GtkWidget *level_bar;
    GtkWidget *box;
    guint timeout_id;
    guint remaining;
    guint period;
} ValidityWidgets;

static void
validity_widgets_free (ValidityWidgets *widgets)
{
    if (widgets->timeout_id != 0)
        g_source_remove (widgets->timeout_id);

    g_free (widgets);
}

G_GNUC_PRINTF (2, 3)
static void
show_error_toast (OTPClientWindow *self, const gchar *format, ...)
{
    if (self == NULL || self->toast_overlay == NULL)
        return;

    va_list args;
    va_start (args, format);
    g_autofree gchar *msg = g_strdup_vprintf (format, args);
    va_end (args);

    // Mirror to the journal so failures stay debuggable from logs.
    g_warning ("%s", msg);

    AdwToast *toast = adw_toast_new (msg);
    adw_toast_set_timeout (toast, 6);
    adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay), toast);
}

static void
validity_update_display (ValidityWidgets *widgets)
{
    if (widgets == NULL)
        return;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        g_application_get_default ());
    gboolean show_seconds = app != NULL &&
        otpclient_application_get_show_validity_seconds (app);

    if (show_seconds)
    {
        if (widgets->label != NULL && GTK_IS_LABEL (widgets->label))
        {
            gchar label_text[8];
            g_snprintf (label_text, sizeof label_text, "%us", widgets->remaining);
            gtk_label_set_text (GTK_LABEL (widgets->label), label_text);
            gtk_widget_set_visible (widgets->label, TRUE);
        }
        if (widgets->level_bar != NULL)
            gtk_widget_set_visible (widgets->level_bar, FALSE);
    }
    else
    {
        if (widgets->label != NULL)
            gtk_widget_set_visible (widgets->label, FALSE);
        if (widgets->level_bar != NULL && GTK_IS_LEVEL_BAR (widgets->level_bar))
        {
            gdouble fraction = (widgets->period > 0)
                ? (gdouble) widgets->remaining / widgets->period
                : 0.0;
            gtk_level_bar_set_value (GTK_LEVEL_BAR (widgets->level_bar), fraction);

            /* Apply color based on remaining time */
            const gchar *color;
            if (widgets->remaining <= widgets->period / 4)
                color = otpclient_application_get_validity_warning_color (app);
            else
                color = otpclient_application_get_validity_color (app);

            GtkCssProvider *provider = g_object_get_data (G_OBJECT (widgets->level_bar), "css-provider");
            if (provider == NULL)
            {
                provider = gtk_css_provider_new ();
                g_object_set_data_full (G_OBJECT (widgets->level_bar), "css-provider",
                                        provider, g_object_unref);
                gtk_widget_add_css_class (widgets->level_bar, "otp-validity");
                gtk_style_context_add_provider_for_display (
                    gtk_widget_get_display (widgets->level_bar),
                    GTK_STYLE_PROVIDER (provider),
                    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
                g_object_set_data (G_OBJECT (widgets->level_bar), "last-color", NULL);
            }
            const gchar *last_color = g_object_get_data (G_OBJECT (widgets->level_bar), "last-color");
            if (last_color == NULL || g_strcmp0 (last_color, color) != 0)
            {
                g_autofree gchar *css = g_strdup_printf (
                    "levelbar.otp-validity trough block.filled { background-color: %s; }", color);
                gtk_css_provider_load_from_string (provider, css);
                g_object_set_data_full (G_OBJECT (widgets->level_bar), "last-color",
                                        g_strdup (color), g_free);
            }

            gtk_widget_set_visible (widgets->level_bar, TRUE);
        }
    }
}

static gboolean
validity_tick (gpointer data)
{
    GtkListItem *list_item = GTK_LIST_ITEM (data);
    ValidityWidgets *widgets;

    if (!GTK_IS_LIST_ITEM (list_item))
        return G_SOURCE_REMOVE;

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
    validity_update_display (widgets);

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
        OTPEntry *entry = gtk_list_item_get_item (list_item);
        guint32 period = 30;
        if (entry != NULL)
            period = otp_entry_get_period (entry);

        gint64 now = g_get_real_time () / G_USEC_PER_SEC;
        widgets->period = period;
        widgets->remaining = period - (guint32)(now % period);
        validity_update_display (widgets);
        gtk_widget_set_visible (widgets->box, TRUE);
        widgets->timeout_id = g_timeout_add_seconds_full (G_PRIORITY_DEFAULT,
                                                          1,
                                                          validity_tick,
                                                          g_object_ref (list_item),
                                                          g_object_unref);
    }
    else
    {
        gtk_widget_set_visible (widgets->box, FALSE);
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

    g_object_set_data (G_OBJECT (label), "otp-entry", entry);

    switch (column)
    {
        case OTP_COLUMN_ACCOUNT:
            text = otp_entry_get_account (entry);
            break;
        case OTP_COLUMN_ISSUER:
            text = otp_entry_get_issuer (entry);
            break;
        case OTP_COLUMN_VALUE:
            text = otp_entry_get_otp_value (entry);
            break;
        default:
            break;
    }

    if (column == OTP_COLUMN_VALUE)
    {
        GtkWidget *toplevel = GTK_WIDGET (gtk_widget_get_root (label));
        if (toplevel != NULL && OTPCLIENT_IS_WINDOW (toplevel))
        {
            OTPClientApplication *app = OTPCLIENT_APPLICATION (
                gtk_window_get_application (GTK_WINDOW (toplevel)));
            if (app != NULL && otpclient_application_get_show_next_otp (app))
            {
                g_autofree gchar *next_otp = otp_entry_get_next_otp (entry);
                if (next_otp != NULL)
                {
                    g_autofree gchar *combined = g_strdup_printf ("%s  [%s]",
                                                                   text ? text : "", next_otp);
                    gtk_label_set_text (GTK_LABEL (label), combined);
                    return;
                }
            }
        }
    }

    /* Show DB name badge for cross-database entries in the Account column */
    const gchar *db_name = otp_entry_get_db_name (entry);
    if (column == OTP_COLUMN_ACCOUNT && db_name != NULL)
    {
        gtk_label_set_use_markup (GTK_LABEL (label), TRUE);
        g_autofree gchar *escaped_text = g_markup_escape_text (text ? text : "", -1);
        g_autofree gchar *escaped_db = g_markup_escape_text (db_name, -1);
        g_autofree gchar *markup = g_strdup_printf (
            "%s  <span size=\"small\" alpha=\"60%%\">[%s]</span>",
            escaped_text, escaped_db);
        gtk_label_set_markup (GTK_LABEL (label), markup);
        return;
    }

    gtk_label_set_use_markup (GTK_LABEL (label), FALSE);
    gtk_label_set_text (GTK_LABEL (label), text ? text : "");
}

static void
otp_validity_column_setup (GtkSignalListItemFactory *factory,
                           GtkListItem              *list_item,
                           gpointer                  user_data)
{
    (void) factory;
    (void) user_data;

    ValidityWidgets *widgets = g_new0 (ValidityWidgets, 1);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_visible (box, FALSE);
    gtk_widget_set_valign (box, GTK_ALIGN_CENTER);

    GtkWidget *label = gtk_label_new (NULL);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
    gtk_widget_set_visible (label, FALSE);
    gtk_box_append (GTK_BOX (box), label);

    GtkWidget *level_bar = gtk_level_bar_new_for_interval (0.0, 1.0);
    gtk_level_bar_set_mode (GTK_LEVEL_BAR (level_bar), GTK_LEVEL_BAR_MODE_CONTINUOUS);
    gtk_widget_set_visible (level_bar, FALSE);
    gtk_widget_set_size_request (level_bar, 60, 8);
    gtk_box_append (GTK_BOX (box), level_bar);

    gtk_list_item_set_child (list_item, box);

    widgets->box = box;
    widgets->label = label;
    widgets->level_bar = level_bar;
    widgets->remaining = 30;
    widgets->period = 30;
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

    gtk_column_view_column_set_expand (view_column, TRUE);
    gtk_column_view_column_set_resizable (view_column, TRUE);

    /* Add sorters for Account and Issuer columns */
    if (column == OTP_COLUMN_ACCOUNT)
    {
        GtkExpression *expr = gtk_property_expression_new (OTP_TYPE_ENTRY, NULL, "account");
        GtkStringSorter *sorter = gtk_string_sorter_new (expr);
        gtk_string_sorter_set_ignore_case (sorter, TRUE);
        gtk_column_view_column_set_sorter (view_column, GTK_SORTER (sorter));
        g_object_unref (sorter);
    }
    else if (column == OTP_COLUMN_ISSUER)
    {
        GtkExpression *expr = gtk_property_expression_new (OTP_TYPE_ENTRY, NULL, "issuer");
        GtkStringSorter *sorter = gtk_string_sorter_new (expr);
        gtk_string_sorter_set_ignore_case (sorter, TRUE);
        gtk_column_view_column_set_sorter (view_column, GTK_SORTER (sorter));
        g_object_unref (sorter);
    }

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

    gtk_column_view_column_set_fixed_width (view_column, 80);
    gtk_column_view_column_set_resizable (view_column, FALSE);

    gtk_column_view_append_column (view, view_column);
}

static gboolean
otp_refresh_tick (gpointer user_data)
{
    OTPClientWindow *self = OTPCLIENT_WINDOW (user_data);

    if (self->otp_store == NULL)
        return G_SOURCE_REMOVE;

    guint n = g_list_model_get_n_items (G_LIST_MODEL (self->otp_store));
    for (guint i = 0; i < n; i++)
    {
        g_autoptr (OTPEntry) entry = g_list_model_get_item (G_LIST_MODEL (self->otp_store), i);
        if (entry == NULL)
            continue;

        const gchar *type = otp_entry_get_otp_type (entry);
        if (type != NULL && g_ascii_strcasecmp (type, "TOTP") == 0)
        {
            guint32 period = otp_entry_get_period (entry);
            if (period > 0)
            {
                gint64 now = g_get_real_time () / G_USEC_PER_SEC;
                guint32 remaining = period - (guint32)(now % period);
                /* Recompute OTP when validity just reset */
                if (remaining == period)
                    otp_entry_update_otp (entry);
            }
        }
    }

    return G_SOURCE_CONTINUE;
}

void
otpclient_window_start_otp_timer (OTPClientWindow *self)
{
    g_return_if_fail (OTPCLIENT_IS_WINDOW (self));

    if (self->otp_refresh_timer_id != 0)
        return;

    self->otp_refresh_timer_id = g_timeout_add_seconds (1, otp_refresh_tick, self);
}

void
otpclient_window_stop_otp_timer (OTPClientWindow *self)
{
    g_return_if_fail (OTPCLIENT_IS_WINDOW (self));

    if (self->otp_refresh_timer_id != 0)
    {
        g_source_remove (self->otp_refresh_timer_id);
        self->otp_refresh_timer_id = 0;
    }
}

/* ── Cross-database search ─────────────────────────────────────────── */

static void
cross_db_ensure_flatten_model (OTPClientWindow *self)
{
    if (self->flatten_model != NULL)
        return;

    GListStore *models = g_list_store_new (G_TYPE_LIST_MODEL);
    g_list_store_append (models, G_LIST_MODEL (self->otp_store));
    if (self->cross_db_store != NULL)
        g_list_store_append (models, G_LIST_MODEL (self->cross_db_store));

    self->flatten_model = gtk_flatten_list_model_new (G_LIST_MODEL (models));
}

static void
cross_db_activate (OTPClientWindow *self)
{
    cross_db_ensure_flatten_model (self);
    gtk_filter_list_model_set_model (self->filter_model, G_LIST_MODEL (self->flatten_model));
}

static void
cross_db_deactivate (OTPClientWindow *self)
{
    gtk_filter_list_model_set_model (self->filter_model, G_LIST_MODEL (self->otp_store));
}

typedef struct {
    GPtrArray *db_list;    /* array of DbListEntry (owned) */
    gchar *current_db_path;
    gint32 max_file_size;
} CrossDbTaskData;

static void
cross_db_task_data_free (gpointer data)
{
    CrossDbTaskData *td = data;
    g_free (td->current_db_path);
    if (td->db_list != NULL)
        g_ptr_array_unref (td->db_list);
    g_free (td);
}

static void
cross_db_load_thread (GTask        *task,
                      gpointer      source,
                      gpointer      task_data,
                      GCancellable *cancellable)
{
    (void) source;
    (void) cancellable;

    CrossDbTaskData *td = task_data;
    GListStore *result = g_list_store_new (OTP_TYPE_ENTRY);

    for (guint i = 0; i < td->db_list->len; i++)
    {
        DbListEntry *dbe = g_ptr_array_index (td->db_list, i);

        /* Skip the currently active database */
        if (g_strcmp0 (dbe->path, td->current_db_path) == 0)
            continue;

        /* Look up password from Secret Service */
        gchar *pwd = secret_password_lookup_sync (OTPCLIENT_SCHEMA, NULL, NULL,
                                                   "string", dbe->path, NULL);
        if (pwd == NULL)
            continue;   /* No password stored for this DB -- skip it */

        DatabaseData *db_data = g_new0 (DatabaseData, 1);
        db_data->db_path = g_strdup (dbe->path);
        db_data->key = secure_strdup (pwd);
        secret_password_free (pwd);
        db_data->max_file_size_from_memlock = td->max_file_size;

        GError *err = NULL;
        load_db (db_data, &err);
        if (err != NULL || db_data->in_memory_json_data == NULL)
        {
            if (err != NULL)
                g_clear_error (&err);
            db_invalidate_kdf_cache (db_data);
            gcry_free (db_data->key);
            g_slist_free_full (db_data->objects_hash, g_free);
            g_free (db_data->db_path);
            g_free (db_data);
            continue;
        }

        gsize idx;
        json_t *obj;
        json_array_foreach (db_data->in_memory_json_data, idx, obj)
        {
            const gchar *label = json_string_value (json_object_get (obj, "label"));
            const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
            const gchar *type = json_string_value (json_object_get (obj, "type"));
            const gchar *algo = json_string_value (json_object_get (obj, "algo"));
            const gchar *secret_str = json_string_value (json_object_get (obj, "secret"));
            gint64 period = json_integer_value (json_object_get (obj, "period"));
            gint64 counter = json_integer_value (json_object_get (obj, "counter"));
            gint64 digits = json_integer_value (json_object_get (obj, "digits"));

            if (period <= 0) period = 30;
            if (digits <= 0) digits = 6;
            if (type == NULL) type = "TOTP";
            if (algo == NULL) algo = "SHA1";

            OTPEntry *entry = otp_entry_new (
                label ? label : "",
                issuer ? issuer : "",
                NULL,
                type,
                (guint32) period,
                (guint64) counter,
                algo,
                (guint32) digits,
                secret_str);

            const gchar *group = json_string_value (json_object_get (obj, "group"));
            if (group != NULL)
                otp_entry_set_group (entry, group);
            otp_entry_set_db_name (entry, dbe->name);
            otp_entry_update_otp (entry);
            g_list_store_append (result, entry);
            g_object_unref (entry);
        }

        db_invalidate_kdf_cache (db_data);
        gcry_free (db_data->key);
        json_decref (db_data->in_memory_json_data);
        g_slist_free_full (db_data->objects_hash, g_free);
        g_free (db_data->db_path);
        g_free (db_data);
    }

    g_task_return_pointer (task, result, g_object_unref);
}

static void
cross_db_load_done (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
    (void) source;

    OTPClientWindow *self = OTPCLIENT_WINDOW (user_data);
    self->cross_db_loading = FALSE;

    GListStore *store = g_task_propagate_pointer (G_TASK (result), NULL);
    if (store == NULL)
        return;

    g_clear_object (&self->cross_db_store);
    self->cross_db_store = store;
    self->cross_db_loaded = TRUE;

    /* Rebuild the flatten model with the new cross-db store */
    g_clear_object (&self->flatten_model);

    /* If there's an active search, wire up the flatten model */
    const gchar *text = gtk_editable_get_text (GTK_EDITABLE (self->search_entry));
    if (text != NULL && text[0] != '\0')
    {
        cross_db_activate (self);
        gtk_filter_changed (GTK_FILTER (self->search_filter), GTK_FILTER_CHANGE_DIFFERENT);
    }
}

static void
cross_db_trigger_load (OTPClientWindow *self)
{
    if (self->cross_db_loading)
        return;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL || !otpclient_application_get_use_secret_service (app))
        return;

    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL)
        return;

    GPtrArray *db_list = gsettings_common_get_db_list ();
    if (db_list == NULL || db_list->len <= 1)
    {
        if (db_list != NULL)
            g_ptr_array_unref (db_list);
        return;
    }

    self->cross_db_loading = TRUE;

    CrossDbTaskData *td = g_new0 (CrossDbTaskData, 1);
    td->db_list = db_list;
    td->current_db_path = g_strdup (db_data->db_path);
    td->max_file_size = db_data->max_file_size_from_memlock;

    GTask *task = g_task_new (NULL, NULL, cross_db_load_done, self);
    g_task_set_task_data (task, td, cross_db_task_data_free);
    g_task_run_in_thread (task, cross_db_load_thread);
    g_object_unref (task);
}

void
otpclient_window_invalidate_cross_db (OTPClientWindow *self)
{
    g_return_if_fail (OTPCLIENT_IS_WINDOW (self));
    self->cross_db_loaded = FALSE;
    if (self->cross_db_store != NULL)
        g_list_store_remove_all (self->cross_db_store);
}

static gboolean
search_filter_func (gpointer item,
                    gpointer user_data)
{
    OTPClientWindow *self = OTPCLIENT_WINDOW (user_data);
    OTPEntry *entry = OTP_ENTRY (item);

    /* --- Group dropdown filter --- */
    if (self->active_group_filter != NULL)
    {
        const gchar *entry_group = otp_entry_get_group (entry);
        if (self->active_group_filter[0] == '\0')
        {
            /* "Ungrouped" sentinel: only show entries with no group */
            if (entry_group != NULL && entry_group[0] != '\0')
                return FALSE;
        }
        else
        {
            /* Specific group: must match exactly */
            if (g_strcmp0 (entry_group, self->active_group_filter) != 0)
                return FALSE;
        }
    }

    /* --- Search text filter --- */
    const gchar *search_text = gtk_editable_get_text (GTK_EDITABLE (self->search_entry));
    if (search_text == NULL || search_text[0] == '\0')
        return TRUE;

    /* Parse "group:xxx" or "#xxx" prefix */
    const gchar *remaining_text = search_text;
    g_autofree gchar *search_group = NULL;

    if (g_str_has_prefix (search_text, "group:"))
    {
        const gchar *rest = search_text + 6;
        const gchar *space = strchr (rest, ' ');
        if (space != NULL)
        {
            search_group = g_strndup (rest, space - rest);
            remaining_text = space + 1;
        }
        else
        {
            search_group = g_strdup (rest);
            remaining_text = NULL;
        }
    }
    else if (search_text[0] == '#' && search_text[1] != '\0')
    {
        const gchar *rest = search_text + 1;
        const gchar *space = strchr (rest, ' ');
        if (space != NULL)
        {
            search_group = g_strndup (rest, space - rest);
            remaining_text = space + 1;
        }
        else
        {
            search_group = g_strdup (rest);
            remaining_text = NULL;
        }
    }

    if (search_group != NULL)
    {
        g_autofree gchar *sg_lower = g_utf8_strdown (search_group, -1);
        const gchar *eg_lower = otp_entry_get_group_lower (entry);
        if (g_strstr_len (eg_lower, -1, sg_lower) == NULL)
            return FALSE;

        /* If no remaining text after group prefix, we're done */
        if (remaining_text == NULL || remaining_text[0] == '\0')
            return TRUE;
    }

    /* Account/issuer substring match on remaining text */
    g_autofree gchar *search_lower = g_utf8_strdown (remaining_text, -1);
    const gchar *account_lower = otp_entry_get_account_lower (entry);
    const gchar *issuer_lower = otp_entry_get_issuer_lower (entry);

    return (g_strstr_len (account_lower, -1, search_lower) != NULL ||
            g_strstr_len (issuer_lower, -1, search_lower) != NULL);
}

static void
update_empty_state (OTPClientWindow *self)
{
    if (self->content_stack == NULL || self->otp_store == NULL)
        return;

    guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->otp_store));
    gtk_stack_set_visible_child_name (GTK_STACK (self->content_stack),
                                      n_items > 0 ? "list" : "empty");
}

static void
on_otp_store_items_changed (GListModel *model,
                            guint       position,
                            guint       removed,
                            guint       added,
                            gpointer    user_data)
{
    (void) model;
    (void) position;
    (void) removed;
    (void) added;
    update_empty_state (OTPCLIENT_WINDOW (user_data));
}

void
otpclient_window_show_loading (OTPClientWindow *self)
{
    g_return_if_fail (OTPCLIENT_IS_WINDOW (self));
    if (self->content_stack == NULL)
        return;
    gtk_stack_set_visible_child_name (GTK_STACK (self->content_stack), "loading");
}

void
otpclient_window_hide_loading (OTPClientWindow *self)
{
    g_return_if_fail (OTPCLIENT_IS_WINDOW (self));
    update_empty_state (self);
}

static void
setup_otp_view (OTPClientWindow *self)
{
    self->otp_store = G_LIST_STORE (g_object_ref_sink (g_list_store_new (OTP_TYPE_ENTRY)));
    g_signal_connect (self->otp_store, "items-changed",
                      G_CALLBACK (on_otp_store_items_changed), self);

    self->search_filter = gtk_custom_filter_new (search_filter_func, self, NULL);
    self->filter_model = gtk_filter_list_model_new (G_LIST_MODEL (self->otp_store),
                                                     GTK_FILTER (self->search_filter));

    add_text_column (GTK_COLUMN_VIEW (self->otp_list), _("Account"), OTP_COLUMN_ACCOUNT);
    add_text_column (GTK_COLUMN_VIEW (self->otp_list), _("Issuer"), OTP_COLUMN_ISSUER);
    add_text_column (GTK_COLUMN_VIEW (self->otp_list), _("OTP Value"), OTP_COLUMN_VALUE);
    add_validity_column (GTK_COLUMN_VIEW (self->otp_list));

    GtkSorter *column_sorter = gtk_column_view_get_sorter (GTK_COLUMN_VIEW (self->otp_list));
    self->sort_model = gtk_sort_list_model_new (g_object_ref (G_LIST_MODEL (self->filter_model)),
                                                 column_sorter ? g_object_ref (column_sorter) : NULL);

    self->otp_selection = gtk_single_selection_new (g_object_ref (G_LIST_MODEL (self->sort_model)));

    gtk_single_selection_set_autoselect (self->otp_selection, FALSE);
    gtk_single_selection_set_can_unselect (self->otp_selection, TRUE);

    gtk_column_view_set_model (GTK_COLUMN_VIEW (self->otp_list), GTK_SELECTION_MODEL (self->otp_selection));
}

static void
action_hide_and_unselect (GtkWidget  *widget,
                          const char *action_name,
                          GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);

    /* Clear all displayed OTP values */
    guint n = g_list_model_get_n_items (G_LIST_MODEL (self->otp_store));
    for (guint i = 0; i < n; i++)
    {
        g_autoptr (OTPEntry) entry = g_list_model_get_item (G_LIST_MODEL (self->otp_store), i);
        if (entry != NULL)
            otp_entry_set_otp_value (entry, "");
    }

    /* Unselect all rows */
    gtk_single_selection_set_selected (self->otp_selection, GTK_INVALID_LIST_POSITION);
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
    (void) entry;

    const gchar *text = gtk_editable_get_text (GTK_EDITABLE (win->search_entry));
    gboolean searching = (text != NULL && text[0] != '\0');

    if (searching)
    {
        /* Trigger cross-DB load if needed */
        if (!win->cross_db_loaded && !win->cross_db_loading)
            cross_db_trigger_load (win);

        /* Switch to flatten model if cross-DB data is available */
        if (win->cross_db_loaded && win->cross_db_store != NULL
            && g_list_model_get_n_items (G_LIST_MODEL (win->cross_db_store)) > 0)
            cross_db_activate (win);
    }
    else
    {
        /* Search cleared: revert to single-DB model */
        cross_db_deactivate (win);
    }

    /* Sync search group prefix → dropdown */
    if (!win->syncing_group_filter && win->group_list_model != NULL)
    {
        g_autofree gchar *search_group = NULL;
        if (g_str_has_prefix (text, "group:"))
        {
            const gchar *rest = text + 6;
            const gchar *space = strchr (rest, ' ');
            search_group = space ? g_strndup (rest, space - rest) : g_strdup (rest);
        }
        else if (text[0] == '#' && text[1] != '\0')
        {
            const gchar *rest = text + 1;
            const gchar *space = strchr (rest, ' ');
            search_group = space ? g_strndup (rest, space - rest) : g_strdup (rest);
        }

        if (search_group != NULL)
        {
            /* Find matching group in dropdown and select it */
            guint n_items = g_list_model_get_n_items (G_LIST_MODEL (win->group_list_model));
            g_autofree gchar *sg_lower = g_utf8_strdown (search_group, -1);
            win->syncing_group_filter = TRUE;
            gboolean found = FALSE;
            for (guint i = 1; i < n_items - 1; i++)
            {
                const gchar *item = gtk_string_list_get_string (win->group_list_model, i);
                g_autofree gchar *item_lower = g_utf8_strdown (item, -1);
                if (g_strstr_len (item_lower, -1, sg_lower) != NULL)
                {
                    gtk_drop_down_set_selected (GTK_DROP_DOWN (win->group_dropdown), i);
                    found = TRUE;
                    break;
                }
            }
            if (!found)
                gtk_drop_down_set_selected (GTK_DROP_DOWN (win->group_dropdown), 0);
            win->syncing_group_filter = FALSE;
        }
        else if (!searching)
        {
            /* Search cleared: reset dropdown to "All" */
            win->syncing_group_filter = TRUE;
            g_clear_pointer (&win->active_group_filter, g_free);
            gtk_drop_down_set_selected (GTK_DROP_DOWN (win->group_dropdown), 0);
            win->syncing_group_filter = FALSE;
        }
    }

    gtk_filter_changed (GTK_FILTER (win->search_filter), GTK_FILTER_CHANGE_DIFFERENT);

    /* Auto-select when search narrows to a single result */
    guint n = g_list_model_get_n_items (G_LIST_MODEL (win->filter_model));
    if (n == 1)
    {
        win->suppress_selection_action = TRUE;
        gtk_single_selection_set_selected (win->otp_selection, 0);
        win->suppress_selection_action = FALSE;
    }
}

static void on_otp_selection_changed (GtkSingleSelection *selection,
                                      GParamSpec         *pspec,
                                      OTPClientWindow    *self);

static void
search_entry_activate (GtkEntry        *entry,
                       OTPClientWindow *self)
{
    (void) entry;

    /* Enter in search bar: copy selected OTP and close search.
     * Suppress selection actions while closing the search bar so the
     * filter change does not re-trigger a copy/notification. */
    on_otp_selection_changed (self->otp_selection, NULL, self);
    self->suppress_selection_action = TRUE;
    gtk_search_bar_set_search_mode (GTK_SEARCH_BAR (self->search_bar), FALSE);
    self->suppress_selection_action = FALSE;
}

static void
database_row_selected (GtkListBox      *box,
                       GtkListBoxRow   *row,
                       OTPClientWindow *self)
{
    (void) box;
    (void) self;

    if (row == NULL)
        return;

    /* The application handles DB switching via the selected row's DatabaseEntry */
}

static void on_db_entry_name_changed (DatabaseEntry *entry,
                                      GParamSpec    *pspec,
                                      AdwActionRow  *row);

static void
on_db_entry_primary_changed (DatabaseEntry *entry,
                             GParamSpec    *pspec,
                             GtkWidget     *icon)
{
    (void) pspec;
    gtk_widget_set_visible (icon, database_entry_get_primary (entry));
}

static GtkWidget *
create_database_row (gpointer item,
                     gpointer user_data)
{
    (void) user_data;
    DatabaseEntry *entry = DATABASE_ENTRY (item);

    AdwActionRow *row = ADW_ACTION_ROW (adw_action_row_new ());
    g_object_set (row, "title", database_entry_get_name (entry), NULL);

    const gchar *path = database_entry_get_path (entry);
    if (path != NULL)
        g_object_set (row, "subtitle", path, NULL);

    GtkWidget *check_icon = gtk_image_new_from_icon_name ("object-select-symbolic");
    gtk_widget_set_valign (check_icon, GTK_ALIGN_CENTER);
    gtk_widget_set_visible (check_icon, database_entry_get_primary (entry));
    adw_action_row_add_suffix (row, check_icon);

    g_signal_connect_object (entry, "notify::primary",
                             G_CALLBACK (on_db_entry_primary_changed), check_icon, 0);

    g_signal_connect_object (entry, "notify::name",
                             G_CALLBACK (on_db_entry_name_changed), row, 0);

    return GTK_WIDGET (row);
}

static void
setup_database_list (OTPClientWindow *self)
{
    self->db_store = g_list_store_new (DATABASE_TYPE_ENTRY);

    gtk_list_box_bind_model (GTK_LIST_BOX (self->database_list),
                             G_LIST_MODEL (self->db_store),
                             create_database_row,
                             NULL, NULL);

    g_signal_connect (self->database_list, "row-selected", G_CALLBACK (database_row_selected), self);
}

static void
sync_primary_flags (OTPClientWindow *self)
{
    g_autofree gchar *primary_path = gui_misc_get_db_path_from_cfg ();
    guint n = g_list_model_get_n_items (G_LIST_MODEL (self->db_store));
    for (guint i = 0; i < n; i++)
    {
        g_autoptr (DatabaseEntry) entry = g_list_model_get_item (G_LIST_MODEL (self->db_store), i);
        gboolean is_primary = g_strcmp0 (database_entry_get_path (entry), primary_path) == 0;
        database_entry_set_primary (entry, is_primary);
    }
}

void
otpclient_window_add_database (OTPClientWindow *self,
                               const gchar     *name,
                               const gchar     *path)
{
    g_return_if_fail (OTPCLIENT_IS_WINDOW (self));

    if (!gui_misc_add_db_to_list (self->db_store, name, path))
        return;

    /* If this is the first database, set it as primary */
    if (g_list_model_get_n_items (G_LIST_MODEL (self->db_store)) == 1)
        gui_misc_save_db_path_to_cfg (path);

    sync_primary_flags (self);
}

GListStore *
otpclient_window_get_db_store (OTPClientWindow *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_WINDOW (self), NULL);
    return self->db_store;
}

gint
otpclient_window_get_selected_db_index (OTPClientWindow *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_WINDOW (self), -1);

    GtkListBoxRow *row = gtk_list_box_get_selected_row (GTK_LIST_BOX (self->database_list));
    if (row == NULL)
        return -1;

    return gtk_list_box_row_get_index (row);
}

void
otpclient_window_select_database (OTPClientWindow *self,
                                  gint             index)
{
    g_return_if_fail (OTPCLIENT_IS_WINDOW (self));

    if (index < 0)
        return;

    GtkListBoxRow *row = gtk_list_box_get_row_at_index (
        GTK_LIST_BOX (self->database_list), index);
    if (row != NULL)
        gtk_list_box_select_row (GTK_LIST_BOX (self->database_list), row);
}

static gboolean
clipboard_clear_cb (gpointer user_data)
{
    OTPClientWindow *self = OTPCLIENT_WINDOW (user_data);
    GdkClipboard *clipboard = gdk_display_get_clipboard (gdk_display_get_default ());
    gdk_clipboard_set_text (clipboard, "");
    self->clipboard_clear_timer_id = 0;
    return G_SOURCE_REMOVE;
}

static void
on_otp_selection_changed (GtkSingleSelection *selection,
                          GParamSpec         *pspec,
                          OTPClientWindow    *self)
{
    (void) pspec;

    guint pos = gtk_single_selection_get_selected (selection);
    if (pos == GTK_INVALID_LIST_POSITION)
        return;

    if (self->suppress_selection_action)
        return;

    OTPEntry *entry = OTP_ENTRY (gtk_single_selection_get_selected_item (selection));
    if (entry == NULL)
        return;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));

    /* For HOTP tokens, increment counter and regenerate OTP on each selection.
     * Skip for cross-DB entries since their database is not actively loaded. */
    const gchar *type = otp_entry_get_otp_type (entry);
    if (type != NULL && g_ascii_strcasecmp (type, "HOTP") == 0
        && app != NULL && otp_entry_get_db_name (entry) == NULL)
    {
        DatabaseData *db_data = otpclient_application_get_db_data (app);
        if (db_data != NULL && db_data->in_memory_json_data != NULL)
        {
            guint64 new_counter = otp_entry_get_counter (entry) + 1;
            otp_entry_set_counter (entry, new_counter);
            otp_entry_update_otp (entry);

            json_t *token_obj = json_array_get (db_data->in_memory_json_data, pos);
            if (token_obj != NULL)
            {
                json_object_set_new (token_obj, "counter", json_integer ((json_int_t) new_counter));
                GError *err = NULL;
                update_db (db_data, &err);
                if (err != NULL)
                {
                    g_warning ("Failed to persist HOTP counter: %s", err->message);
                    g_clear_error (&err);
                }
            }
        }
    }

    const gchar *otp_value = otp_entry_get_otp_value (entry);
    if (otp_value == NULL || otp_value[0] == '\0')
        return;

    GdkClipboard *clipboard = gdk_display_get_clipboard (gdk_display_get_default ());
    gdk_clipboard_set_text (clipboard, otp_value);

    /* Schedule clipboard clear */
    if (self->clipboard_clear_timer_id != 0)
        g_source_remove (self->clipboard_clear_timer_id);
    guint clear_timeout = 30;
    if (app != NULL)
        clear_timeout = otpclient_application_get_clipboard_clear_timeout (app);
    if (clear_timeout > 0)
        self->clipboard_clear_timer_id = g_timeout_add_seconds (clear_timeout,
                                                                  clipboard_clear_cb, self);

    /* Send notification unless disabled */
    if (app != NULL && !otpclient_application_get_disable_notifications (app))
    {
        const gchar *issuer = otp_entry_get_issuer (entry);
        g_autofree gchar *body = g_strdup_printf (_("OTP for %s copied to clipboard"),
                                                   issuer ? issuer : otp_entry_get_account (entry));
        g_autoptr (GNotification) notification = g_notification_new ("OTPClient");
        g_notification_set_body (notification, body);
        g_application_send_notification (G_APPLICATION (app), "otp-copied", notification);
    }
}

/* ── Drag-and-drop helpers ─────────────────────────────────────────── */

static guint
find_store_pos_for_entry (OTPClientWindow *self, OTPEntry *entry)
{
    guint n = g_list_model_get_n_items (G_LIST_MODEL (self->otp_store));
    for (guint i = 0; i < n; i++)
    {
        g_autoptr (OTPEntry) e = g_list_model_get_item (G_LIST_MODEL (self->otp_store), i);
        if (e == entry)
            return i;
    }
    return GTK_INVALID_LIST_POSITION;
}

/*
 * Find the OTPEntry and row widget at coordinates (x, y) relative to
 * the GtkColumnView.  Returns the OTPEntry or NULL.
 * If row_out is non-NULL, the row-level widget is written there.
 *
 * The GtkColumnView hierarchy is:
 *   GtkColumnView → GtkColumnListView → GtkColumnViewRowWidget
 *     → GtkColumnViewCell → our child widget (GtkLabel/GtkBox)
 *
 * The row widget is identified as the one whose parent's parent is
 * the GtkColumnView itself.
 */
static OTPEntry *
pick_entry_at (OTPClientWindow *self, double x, double y, GtkWidget **row_out)
{
    GtkWidget *w = gtk_widget_pick (GTK_WIDGET (self->otp_list), x, y,
                                     GTK_PICK_NON_TARGETABLE);
    OTPEntry *entry = NULL;
    GtkWidget *row = NULL;

    while (w != NULL && w != GTK_WIDGET (self->otp_list))
    {
        if (entry == NULL)
        {
            OTPEntry *e = g_object_get_data (G_OBJECT (w), "otp-entry");
            if (e != NULL)
                entry = e;
        }

        if (row == NULL)
        {
            GtkWidget *parent = gtk_widget_get_parent (w);
            if (parent != NULL)
            {
                GtkWidget *gp = gtk_widget_get_parent (parent);
                if (gp == GTK_WIDGET (self->otp_list))
                    row = w;
            }
        }

        w = gtk_widget_get_parent (w);
    }

    if (row_out != NULL)
        *row_out = row;

    return entry;
}

static void
dnd_clear_highlight (OTPClientWindow *self)
{
    if (self->dnd_highlight_row != NULL)
    {
        gtk_widget_remove_css_class (self->dnd_highlight_row, "drop-above");
        gtk_widget_remove_css_class (self->dnd_highlight_row, "drop-below");
        self->dnd_highlight_row = NULL;
    }
}

static GdkContentProvider *
on_drag_prepare (GtkDragSource *source,
                 double         x,
                 double         y,
                 gpointer       user_data)
{
    (void) source;
    (void) x;
    (void) y;

    OTPClientWindow *self = OTPCLIENT_WINDOW (user_data);

    /* Disable DnD while search filter is active */
    const gchar *search_text = gtk_editable_get_text (GTK_EDITABLE (self->search_entry));
    if (search_text != NULL && search_text[0] != '\0')
        return NULL;

    guint pos = gtk_single_selection_get_selected (self->otp_selection);
    if (pos == GTK_INVALID_LIST_POSITION)
        return NULL;

    GValue value = G_VALUE_INIT;
    g_value_init (&value, G_TYPE_UINT);
    g_value_set_uint (&value, pos);

    return gdk_content_provider_new_for_value (&value);
}

static GdkDragAction
on_dnd_motion (GtkDropTarget *target,
               double         x,
               double         y,
               gpointer       user_data)
{
    (void) target;

    OTPClientWindow *self = OTPCLIENT_WINDOW (user_data);

    dnd_clear_highlight (self);

    GtkWidget *row = NULL;
    OTPEntry *entry = pick_entry_at (self, x, y, &row);
    if (entry == NULL || row == NULL)
        return 0;

    graphene_point_t pt;
    if (!gtk_widget_compute_point (row, GTK_WIDGET (self->otp_list),
                                   &GRAPHENE_POINT_INIT (0, 0), &pt))
        return 0;

    gdouble midpoint = pt.y + gtk_widget_get_height (row) / 2.0;

    if (y < midpoint)
        gtk_widget_add_css_class (row, "drop-above");
    else
        gtk_widget_add_css_class (row, "drop-below");

    self->dnd_highlight_row = row;

    return GDK_ACTION_MOVE;
}

static void
on_dnd_leave (GtkDropTarget *target,
              gpointer       user_data)
{
    (void) target;
    OTPClientWindow *self = OTPCLIENT_WINDOW (user_data);
    dnd_clear_highlight (self);
}

static gboolean
on_drop (GtkDropTarget *target,
         const GValue  *value,
         double         x,
         double         y,
         gpointer       user_data)
{
    (void) target;

    OTPClientWindow *self = OTPCLIENT_WINDOW (user_data);

    dnd_clear_highlight (self);

    if (!G_VALUE_HOLDS_UINT (value))
        return FALSE;

    guint source_filter_pos = g_value_get_uint (value);

    /* Resolve source entry from the filter model */
    g_autoptr (OTPEntry) source_entry = g_list_model_get_item (
        G_LIST_MODEL (self->filter_model), source_filter_pos);
    if (source_entry == NULL)
        return FALSE;

    guint source_pos = find_store_pos_for_entry (self, source_entry);
    if (source_pos == GTK_INVALID_LIST_POSITION)
        return FALSE;

    /* Find target entry under the cursor */
    GtkWidget *row = NULL;
    OTPEntry *target_entry = pick_entry_at (self, x, y, &row);
    if (target_entry == NULL)
        return FALSE;

    guint target_pos = find_store_pos_for_entry (self, target_entry);
    if (target_pos == GTK_INVALID_LIST_POSITION)
        return FALSE;

    /* Determine if dropping above or below the target row */
    guint target_adjusted = target_pos;
    if (row != NULL)
    {
        graphene_point_t pt;
        if (gtk_widget_compute_point (row, GTK_WIDGET (self->otp_list),
                                      &GRAPHENE_POINT_INIT (0, 0), &pt))
        {
            gdouble midpoint = pt.y + gtk_widget_get_height (row) / 2.0;
            if (y >= midpoint)
                target_adjusted = target_pos + 1;
        }
    }

    /* Compute insert position after removing the source */
    guint insert_pos;
    if (source_pos < target_adjusted)
        insert_pos = target_adjusted - 1;
    else
        insert_pos = target_adjusted;

    if (insert_pos == source_pos)
        return FALSE;

    /* Reorder in GListStore */
    g_list_store_remove (self->otp_store, source_pos);
    g_list_store_insert (self->otp_store, insert_pos, source_entry);

    /* Reorder in the JSON database */
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app != NULL)
    {
        DatabaseData *db_data = otpclient_application_get_db_data (app);
        if (db_data != NULL && db_data->in_memory_json_data != NULL)
        {
            json_t *arr = db_data->in_memory_json_data;
            json_t *item = json_array_get (arr, source_pos);
            if (item != NULL)
            {
                json_incref (item);
                json_array_remove (arr, source_pos);
                json_array_insert (arr, insert_pos, item);
                json_decref (item);
            }

            GError *err = NULL;
            update_db (db_data, &err);
            if (err != NULL)
            {
                show_error_toast (self, _("Failed to save reordered tokens: %s"), err->message);
                g_clear_error (&err);
            }
        }
    }

    return TRUE;
}

static void
setup_dnd (OTPClientWindow *self)
{
    self->dnd_css_provider = gtk_css_provider_new ();
    gtk_css_provider_load_from_string (self->dnd_css_provider,
        ".drop-above { border-top: 2px solid @accent_color; }"
        ".drop-below { border-bottom: 2px solid @accent_color; }");
    gtk_style_context_add_provider_for_display (
        gtk_widget_get_display (GTK_WIDGET (self)),
        GTK_STYLE_PROVIDER (self->dnd_css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    GtkDragSource *drag_source = gtk_drag_source_new ();
    gtk_drag_source_set_actions (drag_source, GDK_ACTION_MOVE);
    g_signal_connect (drag_source, "prepare", G_CALLBACK (on_drag_prepare), self);
    gtk_widget_add_controller (self->otp_list, GTK_EVENT_CONTROLLER (drag_source));

    GtkDropTarget *drop_target = gtk_drop_target_new (G_TYPE_UINT, GDK_ACTION_MOVE);
    g_signal_connect (drop_target, "drop", G_CALLBACK (on_drop), self);
    g_signal_connect (drop_target, "motion", G_CALLBACK (on_dnd_motion), self);
    g_signal_connect (drop_target, "leave", G_CALLBACK (on_dnd_leave), self);
    gtk_widget_add_controller (self->otp_list, GTK_EVENT_CONTROLLER (drop_target));
}

static void
sidebar_toggle_clicked (GtkToggleButton *button,
                        OTPClientWindow *self)
{
    gboolean active = gtk_toggle_button_get_active (button);
    adw_overlay_split_view_set_show_sidebar (ADW_OVERLAY_SPLIT_VIEW (self->split_view), active);
}

static void
split_view_sidebar_changed (AdwOverlaySplitView *view,
                            GParamSpec          *pspec,
                            OTPClientWindow     *self)
{
    (void) view;
    (void) pspec;

    gboolean show = adw_overlay_split_view_get_show_sidebar (ADW_OVERLAY_SPLIT_VIEW (self->split_view));
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->sidebar_toggle_button), show);
}

static gboolean
on_close_request (GtkWindow       *window,
                  OTPClientWindow *self)
{
    if (self->settings != NULL)
    {
        gint width = gtk_widget_get_width (GTK_WIDGET (window));
        gint height = gtk_widget_get_height (GTK_WIDGET (window));
        if (width > 0 && height > 0)
        {
            g_settings_set_int (self->settings, "window-width", width);
            g_settings_set_int (self->settings, "window-height", height);
        }
        g_settings_set_boolean (self->settings, "show-sidebar",
                                adw_overlay_split_view_get_show_sidebar (ADW_OVERLAY_SPLIT_VIEW (self->split_view)));
    }
    return FALSE;
}

static void
otpclient_window_dispose (GObject *object)
{
    OTPClientWindow *win = OTPCLIENT_WINDOW(object);



    if (win->deleted_token != NULL)
    {
        json_decref (win->deleted_token);
        win->deleted_token = NULL;
    }

    if (win->clipboard_clear_timer_id != 0)
    {
        g_source_remove (win->clipboard_clear_timer_id);
        win->clipboard_clear_timer_id = 0;
    }

    if (win->otp_refresh_timer_id != 0)
    {
        g_source_remove (win->otp_refresh_timer_id);
        win->otp_refresh_timer_id = 0;
    }

    if (win->otp_list && GTK_IS_COLUMN_VIEW (win->otp_list))
        gtk_column_view_set_model (GTK_COLUMN_VIEW (win->otp_list), NULL);

    if (win->otp_selection)
        gtk_single_selection_set_model (win->otp_selection, NULL);

    g_clear_object (&win->otp_selection);
    g_clear_object (&win->sort_model);
    g_clear_object (&win->filter_model);
    g_clear_object (&win->otp_store);
    g_clear_object (&win->db_store);
    g_clear_object (&win->cross_db_store);
    g_clear_object (&win->flatten_model);
    g_clear_object (&win->group_list_model);
    g_clear_pointer (&win->active_group_filter, g_free);
    g_clear_object (&win->settings);

    if (win->dnd_css_provider != NULL)
    {
        gtk_style_context_remove_provider_for_display (
            gtk_widget_get_display (GTK_WIDGET (win)),
            GTK_STYLE_PROVIDER (win->dnd_css_provider));
        g_clear_object (&win->dnd_css_provider);
    }

    gtk_widget_dispose_template (GTK_WIDGET (object), OTPCLIENT_TYPE_WINDOW);
    G_OBJECT_CLASS (otpclient_window_parent_class)->dispose (object);
}

GListStore *
otpclient_window_get_otp_store (OTPClientWindow *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_WINDOW (self), NULL);
    return self->otp_store;
}

GtkSingleSelection *
otpclient_window_get_otp_selection (OTPClientWindow *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_WINDOW (self), NULL);
    return self->otp_selection;
}

static void
rebuild_group_list (OTPClientWindow *self)
{
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
        return;

    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL || db_data->in_memory_json_data == NULL)
        return;

    /* Collect unique group names */
    GHashTable *groups = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    gsize index;
    json_t *obj;
    json_array_foreach (db_data->in_memory_json_data, index, obj)
    {
        const gchar *group = json_string_value (json_object_get (obj, "group"));
        if (group != NULL && group[0] != '\0')
            g_hash_table_add (groups, g_strdup (group));
    }

    /* Sort group names */
    GList *group_names = g_hash_table_get_keys (groups);
    group_names = g_list_sort (group_names, (GCompareFunc) g_utf8_collate);

    /* Remember current selection */
    g_autofree gchar *prev_filter = g_strdup (self->active_group_filter);

    /* Rebuild the string list */
    if (self->group_list_model != NULL)
        g_clear_object (&self->group_list_model);

    self->group_list_model = gtk_string_list_new (NULL);
    gtk_string_list_append (self->group_list_model, _("All"));
    for (GList *l = group_names; l != NULL; l = l->next)
        gtk_string_list_append (self->group_list_model, (const gchar *) l->data);
    gtk_string_list_append (self->group_list_model, _("Ungrouped"));

    self->syncing_group_filter = TRUE;
    gtk_drop_down_set_model (GTK_DROP_DOWN (self->group_dropdown),
                             G_LIST_MODEL (self->group_list_model));

    /* Restore previous selection if still present */
    guint restore_pos = 0; /* default to "All" */
    if (prev_filter != NULL)
    {
        guint n = g_list_model_get_n_items (G_LIST_MODEL (self->group_list_model));
        for (guint i = 0; i < n; i++)
        {
            const gchar *item = gtk_string_list_get_string (self->group_list_model, i);
            if (prev_filter[0] == '\0' && g_strcmp0 (item, _("Ungrouped")) == 0)
            {
                restore_pos = i;
                break;
            }
            else if (g_strcmp0 (item, prev_filter) == 0)
            {
                restore_pos = i;
                break;
            }
        }
    }
    gtk_drop_down_set_selected (GTK_DROP_DOWN (self->group_dropdown), restore_pos);
    self->syncing_group_filter = FALSE;

    g_list_free (group_names);
    g_hash_table_destroy (groups);
}

void
otpclient_window_rebuild_groups (OTPClientWindow *self)
{
    g_return_if_fail (OTPCLIENT_IS_WINDOW (self));
    rebuild_group_list (self);
}

static void
on_group_dropdown_changed (GtkDropDown     *dropdown,
                           GParamSpec      *pspec,
                           OTPClientWindow *self)
{
    (void) pspec;

    if (self->syncing_group_filter)
        return;

    guint selected = gtk_drop_down_get_selected (dropdown);
    guint n = g_list_model_get_n_items (G_LIST_MODEL (self->group_list_model));

    g_clear_pointer (&self->active_group_filter, g_free);

    if (selected == 0)
    {
        /* "All" — no group filter */
        self->active_group_filter = NULL;
    }
    else if (selected == n - 1)
    {
        /* "Ungrouped" — empty string sentinel */
        self->active_group_filter = g_strdup ("");
    }
    else
    {
        const gchar *group_name = gtk_string_list_get_string (self->group_list_model, selected);
        self->active_group_filter = g_strdup (group_name);
    }

    gtk_filter_changed (GTK_FILTER (self->search_filter), GTK_FILTER_CHANGE_DIFFERENT);
}

static void
on_db_modified (gpointer user_data)
{
    OTPClientWindow *self = OTPCLIENT_WINDOW (user_data);
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
        return;

    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL || db_data->in_memory_json_data == NULL)
        return;

    GListStore *store = self->otp_store;

    /* Suppress selection-change side effects (clipboard copy, notifications)
     * while we tear down and rebuild the store. */
    self->suppress_selection_action = TRUE;

    g_list_store_remove_all (store);

    gsize index;
    json_t *obj;
    json_array_foreach (db_data->in_memory_json_data, index, obj)
    {
        const gchar *type = json_string_value (json_object_get (obj, "type"));
        const gchar *label = json_string_value (json_object_get (obj, "label"));
        const gchar *issuer = json_string_value (json_object_get (obj, "issuer"));
        const gchar *secret = json_string_value (json_object_get (obj, "secret"));
        const gchar *algo = json_string_value (json_object_get (obj, "algo"));
        guint32 digits = (guint32) json_integer_value (json_object_get (obj, "digits"));
        guint32 period = 30;
        guint64 counter = 0;

        if (digits < 4) digits = 6;

        if (type != NULL && g_ascii_strcasecmp (type, "HOTP") == 0)
            counter = (guint64) json_integer_value (json_object_get (obj, "counter"));
        else
            period = (guint32) json_integer_value (json_object_get (obj, "period"));

        if (period < 1) period = 30;

        OTPEntry *entry = otp_entry_new (label, issuer, NULL,
                                          type ? type : "TOTP",
                                          period, counter,
                                          algo ? algo : "SHA1",
                                          digits, secret);
        const gchar *group = json_string_value (json_object_get (obj, "group"));
        if (group != NULL)
            otp_entry_set_group (entry, group);
        otp_entry_update_otp (entry);
        g_list_store_append (store, entry);
        g_object_unref (entry);
    }

    rebuild_group_list (self);

    self->suppress_selection_action = FALSE;
}

static void
on_import_done (const ImportSummary *summary,
                gpointer             user_data)
{
    OTPClientWindow *self = OTPCLIENT_WINDOW (user_data);

    on_db_modified (self);

    if (summary == NULL || self->toast_overlay == NULL)
        return;

    g_autofree gchar *msg = NULL;
    if (summary->added == 0 && summary->skipped == 0) {
        msg = g_strdup (_("No tokens were imported."));
    } else if (summary->skipped == 0) {
        msg = g_strdup_printf (ngettext ("Imported %u token.",
                                         "Imported %u tokens.",
                                         summary->added), summary->added);
    } else if (summary->added == 0) {
        msg = g_strdup_printf (ngettext ("Skipped %u duplicate.",
                                         "Skipped %u duplicates.",
                                         summary->skipped), summary->skipped);
    } else {
        msg = g_strdup_printf (_("Imported %u, skipped %u duplicate%s."),
                               summary->added, summary->skipped,
                               summary->skipped == 1 ? "" : "s");
    }

    AdwToast *toast = adw_toast_new (msg);
    adw_toast_set_timeout (toast, 6);
    adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay), toast);
}

static void
action_add_manual (GtkWidget  *widget,
                   const char *action_name,
                   GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
        return;

    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL)
        return;

    ManualAddDialog *dlg = manual_add_dialog_new (db_data, on_db_modified, self);
    adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self));
}

static void
on_qr_file_selected (GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
    OTPClientWindow *self = OTPCLIENT_WINDOW (user_data);
    GtkFileDialog *dialog = GTK_FILE_DIALOG (source);
    g_autoptr (GFile) file = gtk_file_dialog_open_finish (dialog, result, NULL);
    if (file == NULL)
        return;

    g_autofree gchar *path = g_file_get_path (file);
    if (path == NULL)
        return;

    GError *err = NULL;
    gchar *otpauth_uri = qrcode_parse_image_file (path, &err);
    if (otpauth_uri == NULL) {
        show_error_toast (self, _("Could not read QR code: %s"), err ? err->message : _("unknown error"));
        g_clear_error (&err);
        return;
    }

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL) {
        g_free (otpauth_uri);
        return;
    }

    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL) {
        g_free (otpauth_uri);
        return;
    }

    GSList *otps = NULL;
    set_otps_from_uris (otpauth_uri, &otps);
    g_free (otpauth_uri);

    if (otps != NULL) {
        add_otps_to_db (otps, db_data);
        free_otps_gslist (otps, g_slist_length (otps));
        update_db (db_data, &err);
        if (err != NULL) {
            show_error_toast (self, _("Failed to add scanned token: %s"), err->message);
            g_clear_error (&err);
        } else {
            reload_db (db_data, &err);
            g_clear_error (&err);
        }
        on_db_modified (self);
    }
}

static void
action_add_qr_file (GtkWidget  *widget,
                    const char *action_name,
                    GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    GtkFileDialog *dialog = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dialog, _("Select QR Code Image"));

    GtkFileFilter *filter = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter, _("Image Files"));
    gtk_file_filter_add_mime_type (filter, "image/png");
    gtk_file_filter_add_mime_type (filter, "image/jpeg");
    GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
    g_list_store_append (filters, filter);
    gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
    g_object_unref (filter);
    g_object_unref (filters);

    gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL,
                          on_qr_file_selected, self);
    g_object_unref (dialog);
}

static void
action_add_qr_webcam (GtkWidget  *widget,
                      const char *action_name,
                      GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
        return;

    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL)
        return;

    GError *err = NULL;
    gchar *otpauth_uri = webcam_scan_qrcode (&err);
    if (otpauth_uri == NULL) {
        show_error_toast (self, _("Webcam scan failed: %s"), err ? err->message : _("unknown error"));
        g_clear_error (&err);
        return;
    }

    GSList *otps = NULL;
    set_otps_from_uris (otpauth_uri, &otps);
    g_free (otpauth_uri);

    if (otps != NULL) {
        add_otps_to_db (otps, db_data);
        free_otps_gslist (otps, g_slist_length (otps));
        update_db (db_data, &err);
        if (err != NULL) {
            show_error_toast (self, _("Failed to add scanned token: %s"), err->message);
            g_clear_error (&err);
        } else {
            reload_db (db_data, &err);
            g_clear_error (&err);
        }
        on_db_modified (self);
    }
}

static void
action_import (GtkWidget  *widget,
               const char *action_name,
               GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
        return;

    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL)
        return;

    ImportDialog *dlg = import_dialog_new (db_data, GTK_WIDGET (self),
                                            on_import_done, self);
    adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self));
}

static void
action_export (GtkWidget  *widget,
               const char *action_name,
               GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
        return;

    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL)
        return;

    ExportDialog *dlg = export_dialog_new (db_data, GTK_WIDGET (self));
    adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self));
}

static void
action_settings (GtkWidget  *widget,
                 const char *action_name,
                 GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
        return;

    SettingsDialog *dlg = settings_dialog_new (app);
    adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self));
}

static void
action_edit_token (GtkWidget  *widget,
                   const char *action_name,
                   GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    guint pos = gtk_single_selection_get_selected (self->otp_selection);
    if (pos == GTK_INVALID_LIST_POSITION)
        return;

    /* Cannot edit cross-DB entries */
    OTPEntry *sel_entry = OTP_ENTRY (gtk_single_selection_get_selected_item (self->otp_selection));
    if (sel_entry != NULL && otp_entry_get_db_name (sel_entry) != NULL)
        return;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
        return;

    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL || db_data->in_memory_json_data == NULL)
        return;

    json_t *token_obj = json_array_get (db_data->in_memory_json_data, pos);
    if (token_obj == NULL)
        return;

    EditTokenDialog *dlg = edit_token_dialog_new (token_obj, pos, db_data,
                                                   on_db_modified, self);
    adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self));
}

static void
on_undo_delete (AdwToast *toast,
                gpointer  user_data)
{
    (void) toast;
    OTPClientWindow *self = OTPCLIENT_WINDOW (user_data);

    if (self->deleted_token == NULL)
        return;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
        return;

    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL || db_data->in_memory_json_data == NULL)
        return;

    gsize arr_len = json_array_size (db_data->in_memory_json_data);
    guint insert_pos = (self->deleted_token_pos <= arr_len) ? self->deleted_token_pos : (guint) arr_len;
    json_array_insert (db_data->in_memory_json_data, insert_pos, self->deleted_token);

    json_decref (self->deleted_token);
    self->deleted_token = NULL;

    GError *err = NULL;
    update_db (db_data, &err);
    if (err != NULL) {
        show_error_toast (self, _("Failed to restore deleted token: %s"), err->message);
        g_clear_error (&err);
        return;
    }

    on_db_modified (self);
}

static void
on_delete_response (AdwAlertDialog *dialog,
                    GAsyncResult   *result,
                    gpointer        user_data)
{
    OTPClientWindow *self = OTPCLIENT_WINDOW (user_data);
    const gchar *response = adw_alert_dialog_choose_finish (dialog, result);

    if (g_strcmp0 (response, "delete") != 0)
        return;

    guint pos = gtk_single_selection_get_selected (self->otp_selection);
    if (pos == GTK_INVALID_LIST_POSITION)
        return;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
        return;

    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL || db_data->in_memory_json_data == NULL)
        return;

    /* Save a copy for undo */
    if (self->deleted_token != NULL)
        json_decref (self->deleted_token);
    self->deleted_token = json_deep_copy (json_array_get (db_data->in_memory_json_data, pos));
    self->deleted_token_pos = pos;

    json_array_remove (db_data->in_memory_json_data, pos);

    GError *err = NULL;
    update_db (db_data, &err);
    if (err != NULL) {
        show_error_toast (self, _("Failed to delete token: %s"), err->message);
        g_clear_error (&err);
        json_decref (self->deleted_token);
        self->deleted_token = NULL;
        return;
    }

    on_db_modified (self);

    /* Show undo toast */
    AdwToast *undo_toast = adw_toast_new (_("Token deleted"));
    adw_toast_set_button_label (undo_toast, _("Undo"));
    adw_toast_set_timeout (undo_toast, 5);
    g_signal_connect (undo_toast, "button-clicked", G_CALLBACK (on_undo_delete), self);
    adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay), undo_toast);
}

static void
action_delete_token (GtkWidget  *widget,
                     const char *action_name,
                     GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    guint pos = gtk_single_selection_get_selected (self->otp_selection);
    if (pos == GTK_INVALID_LIST_POSITION)
        return;

    /* Cannot delete cross-DB entries */
    OTPEntry *sel_entry = OTP_ENTRY (gtk_single_selection_get_selected_item (self->otp_selection));
    if (sel_entry != NULL && otp_entry_get_db_name (sel_entry) != NULL)
        return;

    AdwAlertDialog *dialog = ADW_ALERT_DIALOG (
        adw_alert_dialog_new (_("Delete Token?"),
                              _("This action cannot be undone.")));

    adw_alert_dialog_add_responses (dialog,
                                    "cancel", _("Cancel"),
                                    "delete", _("Delete"),
                                    NULL);
    adw_alert_dialog_set_response_appearance (dialog, "delete", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response (dialog, "cancel");

    adw_alert_dialog_choose (dialog, GTK_WIDGET (self), NULL,
                             (GAsyncReadyCallback) on_delete_response, self);
}

static void
action_show_qr (GtkWidget  *widget,
                const char *action_name,
                GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    guint pos = gtk_single_selection_get_selected (self->otp_selection);
    if (pos == GTK_INVALID_LIST_POSITION)
        return;

    /* Cannot show QR for cross-DB entries */
    OTPEntry *sel_entry = OTP_ENTRY (gtk_single_selection_get_selected_item (self->otp_selection));
    if (sel_entry != NULL && otp_entry_get_db_name (sel_entry) != NULL)
        return;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
        return;

    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL || db_data->in_memory_json_data == NULL)
        return;

    json_t *token_obj = json_array_get (db_data->in_memory_json_data, pos);
    if (token_obj == NULL)
        return;

    gchar *otpauth_uri = get_otpauth_uri (token_obj);
    if (otpauth_uri == NULL)
        return;

    const gchar *label = json_string_value (json_object_get (token_obj, "label"));
    QrDisplayDialog *dlg = qr_display_dialog_new (otpauth_uri, label ? label : "Token");
    g_free (otpauth_uri);
    adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self));
}

/* ---- Move token to another database ---- */

typedef struct {
    OTPClientWindow *self;
    guint            token_pos;
    json_t          *token_json;   /* deep copy, owned */
    gchar           *target_db_path;
} MoveTokenContext;

static void
move_token_context_free (MoveTokenContext *ctx)
{
    if (ctx->token_json != NULL)
        json_decref (ctx->token_json);
    g_free (ctx->target_db_path);
    g_free (ctx);
}

static void
on_move_target_password (const gchar *password,
                         gpointer     user_data)
{
    MoveTokenContext *ctx = (MoveTokenContext *) user_data;
    OTPClientWindow *self = ctx->self;

    if (password == NULL)
    {
        move_token_context_free (ctx);
        return;
    }

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
    {
        move_token_context_free (ctx);
        return;
    }

    /* Open and decrypt the target database */
    DatabaseData *target = g_new0 (DatabaseData, 1);
    target->db_path = g_strdup (ctx->target_db_path);
    target->key = gcry_calloc_secure (strlen (password) + 1, 1);
    memcpy (target->key, password, strlen (password) + 1);

    gint32 memlock = 0;
    set_memlock_value (&memlock);
    target->max_file_size_from_memlock = memlock;

    GError *err = NULL;
    load_db (target, &err);
    if (err != NULL)
    {
        show_error_toast (self, _("Could not open target database: %s"), err->message);
        g_clear_error (&err);
        gcry_free (target->key);
        g_free (target->db_path);
        g_free (target);
        move_token_context_free (ctx);
        return;
    }

    /* Append the token to the target database */
    if (target->in_memory_json_data == NULL)
        target->in_memory_json_data = json_array ();

    json_array_append (target->in_memory_json_data, ctx->token_json);

    update_db (target, &err);
    if (err != NULL)
    {
        show_error_toast (self, _("Could not write token to target database: %s"), err->message);
        g_clear_error (&err);
    }
    else
    {
        /* Remove the token from the current (source) database */
        DatabaseData *src = otpclient_application_get_db_data (app);
        if (src != NULL && src->in_memory_json_data != NULL)
        {
            json_array_remove (src->in_memory_json_data, ctx->token_pos);
            update_db (src, &err);
            if (err != NULL)
            {
                show_error_toast (self, _("Could not remove token from source database: %s"), err->message);
                g_clear_error (&err);
            }
            on_db_modified (self);
        }
    }

    /* Clean up target */
    json_decref (target->in_memory_json_data);
    gcry_free (target->key);
    g_free (target->db_path);
    g_slist_free_full (target->objects_hash, g_free);
    g_free (target);

    move_token_context_free (ctx);
}

static void
on_move_db_selected (AdwAlertDialog *dialog,
                     GAsyncResult   *result,
                     gpointer        user_data)
{
    MoveTokenContext *ctx = (MoveTokenContext *) user_data;
    const gchar *response = adw_alert_dialog_choose_finish (dialog, result);

    if (g_strcmp0 (response, "cancel") == 0 || response == NULL)
    {
        move_token_context_free (ctx);
        return;
    }

    /* The response ID is the db_path */
    ctx->target_db_path = g_strdup (response);

    PasswordDialog *pwd_dlg = password_dialog_new (PASSWORD_MODE_DECRYPT,
                                                    on_move_target_password,
                                                    ctx);
    adw_dialog_present (ADW_DIALOG (pwd_dlg), GTK_WIDGET (ctx->self));
}

static void
action_move_token (GtkWidget  *widget,
                   const char *action_name,
                   GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    guint pos = gtk_single_selection_get_selected (self->otp_selection);
    if (pos == GTK_INVALID_LIST_POSITION)
        return;

    /* Cannot move cross-DB entries */
    OTPEntry *sel_entry = OTP_ENTRY (gtk_single_selection_get_selected_item (self->otp_selection));
    if (sel_entry != NULL && otp_entry_get_db_name (sel_entry) != NULL)
        return;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
        return;

    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL || db_data->in_memory_json_data == NULL)
        return;

    /* Build list of other databases */
    guint n_dbs = g_list_model_get_n_items (G_LIST_MODEL (self->db_store));
    if (n_dbs < 2)
    {
        AdwAlertDialog *dlg = ADW_ALERT_DIALOG (
            adw_alert_dialog_new (_("Move Token"),
                                  _("No other databases available. Open or create another database first.")));
        adw_alert_dialog_add_response (dlg, "ok", _("OK"));
        adw_alert_dialog_choose (dlg, GTK_WIDGET (self), NULL, NULL, NULL);
        return;
    }

    json_t *token_obj = json_array_get (db_data->in_memory_json_data, pos);
    if (token_obj == NULL)
        return;

    MoveTokenContext *ctx = g_new0 (MoveTokenContext, 1);
    ctx->self = self;
    ctx->token_pos = pos;
    ctx->token_json = json_deep_copy (token_obj);

    const gchar *account = json_string_value (json_object_get (token_obj, "label"));
    g_autofree gchar *body = g_strdup_printf (_("Select the database to move \"%s\" to:"),
                                               account ? account : _("token"));

    AdwAlertDialog *dialog = ADW_ALERT_DIALOG (
        adw_alert_dialog_new (_("Move Token"), body));
    adw_alert_dialog_add_response (dialog, "cancel", _("Cancel"));

    for (guint i = 0; i < n_dbs; i++)
    {
        g_autoptr (DatabaseEntry) entry = g_list_model_get_item (G_LIST_MODEL (self->db_store), i);
        const gchar *path = database_entry_get_path (entry);

        /* Skip the currently active database */
        if (g_strcmp0 (path, db_data->db_path) == 0)
            continue;

        const gchar *name = database_entry_get_name (entry);
        adw_alert_dialog_add_response (dialog, path, name);
    }

    adw_alert_dialog_set_default_response (dialog, "cancel");

    adw_alert_dialog_choose (dialog, GTK_WIDGET (self), NULL,
                             (GAsyncReadyCallback) on_move_db_selected, ctx);
}

static void
lock_button_clicked (GtkButton       *button,
                     OTPClientWindow *self)
{
    (void) button;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app != NULL)
        g_action_group_activate_action (G_ACTION_GROUP (app), "lock", NULL);
}

static gboolean
on_db_modified_idle (gpointer user_data)
{
    on_db_modified (user_data);
    return G_SOURCE_REMOVE;
}

static void
action_set_group (GtkWidget  *widget,
                  const char *action_name,
                  GVariant   *parameter)
{
    (void) action_name;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    guint pos = gtk_single_selection_get_selected (self->otp_selection);
    if (pos == GTK_INVALID_LIST_POSITION)
        return;

    OTPEntry *sel_entry = OTP_ENTRY (gtk_single_selection_get_selected_item (self->otp_selection));
    if (sel_entry != NULL && otp_entry_get_db_name (sel_entry) != NULL)
        return;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
        return;

    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL || db_data->in_memory_json_data == NULL)
        return;

    json_t *token_obj = json_array_get (db_data->in_memory_json_data, pos);
    if (token_obj == NULL)
        return;

    const gchar *group_name = g_variant_get_string (parameter, NULL);
    if (group_name != NULL && group_name[0] != '\0')
        json_object_set (token_obj, "group", json_string (group_name));
    else
        json_object_del (token_obj, "group");

    GError *err = NULL;
    update_db (db_data, &err);
    if (err != NULL)
    {
        show_error_toast (self, _("Failed to change group: %s"), err->message);
        g_clear_error (&err);
        return;
    }

    /* Defer store rebuild so the popover menu can close cleanly first,
     * avoiding "Broken accounting of active state" warnings. */
    g_idle_add (on_db_modified_idle, self);
}

static void
action_remove_from_group (GtkWidget  *widget,
                          const char *action_name,
                          GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    guint pos = gtk_single_selection_get_selected (self->otp_selection);
    if (pos == GTK_INVALID_LIST_POSITION)
        return;

    OTPEntry *sel_entry = OTP_ENTRY (gtk_single_selection_get_selected_item (self->otp_selection));
    if (sel_entry != NULL && otp_entry_get_db_name (sel_entry) != NULL)
        return;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
        return;

    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL || db_data->in_memory_json_data == NULL)
        return;

    json_t *token_obj = json_array_get (db_data->in_memory_json_data, pos);
    if (token_obj == NULL)
        return;

    json_object_del (token_obj, "group");

    GError *err = NULL;
    update_db (db_data, &err);
    if (err != NULL)
    {
        show_error_toast (self, _("Failed to remove token from group: %s"), err->message);
        g_clear_error (&err);
        return;
    }

    /* Defer store rebuild so the popover menu can close cleanly first. */
    g_idle_add (on_db_modified_idle, self);
}

typedef struct {
    OTPClientWindow *self;
    guint token_pos;
} NewGroupContext;

static void
on_new_group_response (AdwAlertDialog  *dialog,
                       GAsyncResult    *result,
                       NewGroupContext  *ctx)
{
    const gchar *response = adw_alert_dialog_choose_finish (dialog, result);
    if (g_strcmp0 (response, "add") != 0)
    {
        g_free (ctx);
        return;
    }

    GtkWidget *extra = adw_alert_dialog_get_extra_child (dialog);
    const gchar *group_name = gtk_editable_get_text (GTK_EDITABLE (extra));
    if (group_name == NULL || group_name[0] == '\0')
    {
        g_free (ctx);
        return;
    }

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (ctx->self)));
    if (app == NULL)
    {
        g_free (ctx);
        return;
    }

    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL || db_data->in_memory_json_data == NULL)
    {
        g_free (ctx);
        return;
    }

    json_t *token_obj = json_array_get (db_data->in_memory_json_data, ctx->token_pos);
    if (token_obj != NULL)
    {
        json_object_set (token_obj, "group", json_string (group_name));

        GError *err = NULL;
        update_db (db_data, &err);
        if (err != NULL)
        {
            show_error_toast (ctx->self, _("Failed to assign group: %s"), err->message);
            g_clear_error (&err);
        }
        else
        {
            on_db_modified (ctx->self);
        }
    }

    g_free (ctx);
}

static void
action_new_group (GtkWidget  *widget,
                  const char *action_name,
                  GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    guint pos = gtk_single_selection_get_selected (self->otp_selection);
    if (pos == GTK_INVALID_LIST_POSITION)
        return;

    OTPEntry *sel_entry = OTP_ENTRY (gtk_single_selection_get_selected_item (self->otp_selection));
    if (sel_entry != NULL && otp_entry_get_db_name (sel_entry) != NULL)
        return;

    NewGroupContext *ctx = g_new0 (NewGroupContext, 1);
    ctx->self = self;
    ctx->token_pos = pos;

    AdwAlertDialog *dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new (
        _("New Group"), _("Enter a name for the new group:")));
    adw_alert_dialog_add_responses (dialog, "cancel", _("Cancel"), "add", _("Add"), NULL);
    adw_alert_dialog_set_response_appearance (dialog, "add", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response (dialog, "add");

    GtkWidget *entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (entry), _("Group name"));
    gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
    adw_alert_dialog_set_extra_child (dialog, entry);

    adw_alert_dialog_choose (dialog, GTK_WIDGET (self), NULL,
                             (GAsyncReadyCallback) on_new_group_response, ctx);
}

static void on_popover_closed (GtkPopover *popover, gpointer user_data);

static void
on_token_right_click (GtkGestureClick *gesture,
                      gint             n_press,
                      gdouble          x,
                      gdouble          y,
                      OTPClientWindow *self)
{
    (void) n_press;

    /* Reset gesture state before showing popover to avoid
     * "Broken accounting of active state" warnings */
    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    gtk_event_controller_reset (GTK_EVENT_CONTROLLER (gesture));

    /* Select the row under the cursor so a right-click alone is enough. */
    OTPEntry *picked = pick_entry_at (self, x, y, NULL);
    if (picked != NULL)
    {
        guint n = g_list_model_get_n_items (G_LIST_MODEL (self->otp_selection));
        for (guint i = 0; i < n; i++)
        {
            g_autoptr (OTPEntry) e = g_list_model_get_item (
                G_LIST_MODEL (self->otp_selection), i);
            if (e == picked)
            {
                self->suppress_selection_action = TRUE;
                gtk_single_selection_set_selected (self->otp_selection, i);
                self->suppress_selection_action = FALSE;
                break;
            }
        }
    }

    guint pos = gtk_single_selection_get_selected (self->otp_selection);
    if (pos == GTK_INVALID_LIST_POSITION)
        return;

    GtkBuilder *builder = gtk_builder_new_from_resource (
        "/com/github/paolostivanin/OTPClient/ui/context-menus.ui");
    GMenuModel *base_menu = G_MENU_MODEL (gtk_builder_get_object (builder, "token_context_menu"));

    /* Build a copy of the base menu and add "Set Group" submenu */
    GMenu *menu = g_menu_new ();

    /* Copy existing sections from base menu */
    gint n_sections = g_menu_model_get_n_items (base_menu);
    for (gint i = 0; i < n_sections; i++)
    {
        g_autoptr (GMenuLinkIter) iter = g_menu_model_iterate_item_links (base_menu, i);
        while (g_menu_link_iter_next (iter))
        {
            const gchar *link_name = g_menu_link_iter_get_name (iter);
            GMenuModel *link = g_menu_link_iter_get_value (iter);
            if (g_strcmp0 (link_name, G_MENU_LINK_SECTION) == 0)
                g_menu_append_section (menu, NULL, link);
            else if (g_strcmp0 (link_name, G_MENU_LINK_SUBMENU) == 0)
                g_menu_append_submenu (menu, NULL, link);
            g_object_unref (link);
        }
    }

    /* Build "Set Group" submenu dynamically */
    GMenu *group_submenu = g_menu_new ();

    /* Add existing groups */
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app != NULL)
    {
        DatabaseData *db_data = otpclient_application_get_db_data (app);
        if (db_data != NULL && db_data->in_memory_json_data != NULL)
        {
            GHashTable *groups = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
            gsize idx;
            json_t *obj;
            json_array_foreach (db_data->in_memory_json_data, idx, obj)
            {
                const gchar *g = json_string_value (json_object_get (obj, "group"));
                if (g != NULL && g[0] != '\0')
                    g_hash_table_add (groups, g_strdup (g));
            }

            GList *names = g_hash_table_get_keys (groups);
            names = g_list_sort (names, (GCompareFunc) g_utf8_collate);
            if (names != NULL)
            {
                GMenu *existing_section = g_menu_new ();
                for (GList *l = names; l != NULL; l = l->next)
                {
                    GMenuItem *item = g_menu_item_new ((const gchar *) l->data, NULL);
                    g_menu_item_set_action_and_target (item, "win.set-group", "s", (const gchar *) l->data);
                    g_menu_append_item (existing_section, item);
                    g_object_unref (item);
                }
                g_menu_append_section (group_submenu, NULL, G_MENU_MODEL (existing_section));
                g_object_unref (existing_section);
            }

            g_list_free (names);
            g_hash_table_destroy (groups);
        }
    }

    /* Add "New Group..." and "Remove from Group" */
    GMenu *manage_section = g_menu_new ();
    g_menu_append (manage_section, _("New Group\u2026"), "win.new-group");
    g_menu_append (manage_section, _("Remove from Group"), "win.remove-from-group");
    g_menu_append_section (group_submenu, NULL, G_MENU_MODEL (manage_section));
    g_object_unref (manage_section);

    /* Add the group submenu as a new section in the main menu */
    GMenu *group_section = g_menu_new ();
    g_menu_append_submenu (group_section, _("Set Group"), G_MENU_MODEL (group_submenu));
    g_menu_append_section (menu, NULL, G_MENU_MODEL (group_section));
    g_object_unref (group_section);
    g_object_unref (group_submenu);

    GtkWidget *popover = gtk_popover_menu_new_from_model (G_MENU_MODEL (menu));
    gtk_widget_set_parent (popover, self->otp_list);
    GdkRectangle rect = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to (GTK_POPOVER (popover), &rect);
    g_signal_connect (popover, "closed", G_CALLBACK (on_popover_closed), NULL);
    gtk_popover_popup (GTK_POPOVER (popover));

    g_object_unref (menu);
    g_object_unref (builder);
}

static void
on_new_db_password_received (const gchar *password,
                              gpointer     user_data);

typedef struct {
    OTPClientWindow *self;
    gchar *db_path;
} NewDbContext;

static void
on_new_db_file_selected (GObject      *source,
                          GAsyncResult *result,
                          gpointer      user_data)
{
    OTPClientWindow *self = OTPCLIENT_WINDOW (user_data);
    GtkFileDialog *dialog = GTK_FILE_DIALOG (source);
    g_autoptr (GFile) file = gtk_file_dialog_save_finish (dialog, result, NULL);
    if (file == NULL)
        return;

    g_autofree gchar *path = g_file_get_path (file);
    if (path == NULL)
        return;

    /* Ensure .enc extension */
    gchar *db_path;
    if (!g_str_has_suffix (path, ".enc"))
        db_path = g_strconcat (path, ".enc", NULL);
    else
        db_path = g_strdup (path);

    NewDbContext *ctx = g_new0 (NewDbContext, 1);
    ctx->self = self;
    ctx->db_path = db_path;

    PasswordDialog *pwd_dlg = password_dialog_new (PASSWORD_MODE_NEW,
                                                    on_new_db_password_received,
                                                    ctx);
    adw_dialog_present (ADW_DIALOG (pwd_dlg), GTK_WIDGET (self));
}

static void
on_new_db_password_received (const gchar *password,
                              gpointer     user_data)
{
    NewDbContext *ctx = (NewDbContext *) user_data;
    OTPClientWindow *self = ctx->self;
    gchar *db_path = ctx->db_path;
    g_free (ctx);

    if (password == NULL)
    {
        g_free (db_path);
        return;
    }

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
    {
        g_free (db_path);
        return;
    }

    /* Free old db_data if present */
    DatabaseData *old_db = otpclient_application_get_db_data (app);
    if (old_db != NULL)
    {
        otpclient_window_stop_otp_timer (self);
        g_list_store_remove_all (self->otp_store);
    }

    /* Create new DatabaseData */
    DatabaseData *db_data = g_new0 (DatabaseData, 1);
    db_data->db_path = db_path;
    db_data->key = gcry_calloc_secure (strlen (password) + 1, 1);
    memcpy (db_data->key, password, strlen (password) + 1);
    db_data->argon2id_iter = ARGON2ID_DEFAULT_ITER;
    db_data->argon2id_memcost = ARGON2ID_DEFAULT_MC;
    db_data->argon2id_parallelism = ARGON2ID_DEFAULT_PARAL;
    db_data->current_db_version = DB_VERSION;

    gint32 memlock = 0;
    set_memlock_value (&memlock);
    db_data->max_file_size_from_memlock = memlock;

    GError *err = NULL;
    update_db (db_data, &err);
    if (err != NULL)
    {
        show_error_toast (self, _("Could not create database: %s"), err->message);
        g_clear_error (&err);
        db_invalidate_kdf_cache (db_data);
        gcry_free (db_data->key);
        g_free (db_data->db_path);
        g_free (db_data);
        return;
    }

    /* Reload to populate in_memory_json_data */
    load_db (db_data, &err);
    if (err != NULL)
    {
        show_error_toast (self, _("Created database but could not reload it: %s"), err->message);
        g_clear_error (&err);
    }

    otpclient_application_set_db_data (app, db_data);

    /* Save path to config */
    gui_misc_save_db_path_to_cfg (db_path);

    /* Add to sidebar */
    g_autofree gchar *display_name = gui_misc_derive_db_display_name (db_path);
    otpclient_window_add_database (self, display_name, db_path);

    on_db_modified (self);
    otpclient_window_start_otp_timer (self);
}

static void
new_db_button_clicked (GtkButton       *button,
                       OTPClientWindow *self)
{
    (void) button;

    GtkFileDialog *dialog = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dialog, _("Create New Database"));
    gtk_file_dialog_set_initial_name (dialog, "otpclient-db.enc");

    gtk_file_dialog_save (dialog, GTK_WINDOW (self), NULL,
                          on_new_db_file_selected, self);
    g_object_unref (dialog);
}

static void
on_open_db_file_selected (GObject      *source,
                           GAsyncResult *result,
                           gpointer      user_data);

static void
on_open_db_password_received (const gchar *password,
                               gpointer     user_data);

static void
on_open_db_file_selected (GObject      *source,
                           GAsyncResult *result,
                           gpointer      user_data)
{
    OTPClientWindow *self = OTPCLIENT_WINDOW (user_data);
    GtkFileDialog *dialog = GTK_FILE_DIALOG (source);
    g_autoptr (GFile) file = gtk_file_dialog_open_finish (dialog, result, NULL);
    if (file == NULL)
        return;

    g_autofree gchar *path = g_file_get_path (file);
    if (path == NULL)
        return;

    NewDbContext *ctx = g_new0 (NewDbContext, 1);
    ctx->self = self;
    ctx->db_path = g_strdup (path);

    PasswordDialog *pwd_dlg = password_dialog_new (PASSWORD_MODE_DECRYPT,
                                                    on_open_db_password_received,
                                                    ctx);
    adw_dialog_present (ADW_DIALOG (pwd_dlg), GTK_WIDGET (self));
}

static void
on_open_db_password_received (const gchar *password,
                               gpointer     user_data)
{
    NewDbContext *ctx = (NewDbContext *) user_data;
    OTPClientWindow *self = ctx->self;
    gchar *db_path = ctx->db_path;
    g_free (ctx);

    if (password == NULL)
    {
        g_free (db_path);
        return;
    }

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
    {
        g_free (db_path);
        return;
    }

    /* Stop current DB */
    otpclient_window_stop_otp_timer (self);
    g_list_store_remove_all (self->otp_store);

    DatabaseData *db_data = g_new0 (DatabaseData, 1);
    db_data->db_path = db_path;
    db_data->key = gcry_calloc_secure (strlen (password) + 1, 1);
    memcpy (db_data->key, password, strlen (password) + 1);

    gint32 memlock = 0;
    set_memlock_value (&memlock);
    db_data->max_file_size_from_memlock = memlock;

    GError *err = NULL;
    load_db (db_data, &err);
    if (err != NULL)
    {
        show_error_toast (self, _("Could not open database: %s"), err->message);
        g_clear_error (&err);
        db_invalidate_kdf_cache (db_data);
        gcry_free (db_data->key);
        g_free (db_data->db_path);
        g_free (db_data);
        return;
    }

    otpclient_application_set_db_data (app, db_data);

    /* Save path to config */
    gui_misc_save_db_path_to_cfg (db_path);

    /* Add to sidebar */
    g_autofree gchar *display_name = gui_misc_derive_db_display_name (db_path);
    otpclient_window_add_database (self, display_name, db_path);

    on_db_modified (self);
    otpclient_window_start_otp_timer (self);
}

static void
open_db_button_clicked (GtkButton       *button,
                        OTPClientWindow *self)
{
    (void) button;

    GtkFileDialog *dialog = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dialog, _("Open Database"));

    GtkFileFilter *filter = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter, _("OTPClient Database (*.enc)"));
    gtk_file_filter_add_pattern (filter, "*.enc");
    GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
    g_list_store_append (filters, filter);
    gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
    g_object_unref (filter);
    g_object_unref (filters);

    gtk_file_dialog_open (dialog, GTK_WINDOW (self), NULL,
                          on_open_db_file_selected, self);
    g_object_unref (dialog);
}

static gboolean
on_key_pressed_inactivity (GtkEventControllerKey *controller,
                            guint                  keyval,
                            guint                  keycode,
                            GdkModifierType        state,
                            OTPClientWindow       *self)
{
    (void) controller;
    (void) keyval;
    (void) keycode;
    (void) state;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app != NULL)
        lock_app_reset_inactivity (app);

    return FALSE;
}

static void
on_click_inactivity (GtkGestureClick *gesture,
                      gint             n_press,
                      gdouble          x,
                      gdouble          y,
                      OTPClientWindow *self)
{
    (void) gesture;
    (void) n_press;
    (void) x;
    (void) y;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app != NULL)
        lock_app_reset_inactivity (app);
}

static gboolean
popover_unparent_idle (gpointer data)
{
    GtkWidget *popover = GTK_WIDGET (data);
    gtk_widget_unparent (popover);
    g_object_unref (popover);
    return G_SOURCE_REMOVE;
}

static void
on_popover_closed (GtkPopover *popover,
                   gpointer    user_data)
{
    (void) user_data;
    /* Defer unparent so the action activation completes first */
    g_object_ref (popover);
    g_idle_add (popover_unparent_idle, popover);
}

static void
on_db_right_click (GtkGestureClick *gesture,
                   gint             n_press,
                   gdouble          x,
                   gdouble          y,
                   OTPClientWindow *self)
{
    (void) n_press;

    gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
    gtk_event_controller_reset (GTK_EVENT_CONTROLLER (gesture));

    GtkListBoxRow *row = gtk_list_box_get_row_at_y (
        GTK_LIST_BOX (self->database_list), (gint)y);
    if (row == NULL)
        return;

    gtk_list_box_select_row (GTK_LIST_BOX (self->database_list), row);

    GtkBuilder *builder = gtk_builder_new_from_resource (
        "/com/github/paolostivanin/OTPClient/ui/context-menus.ui");
    GMenuModel *menu_model = G_MENU_MODEL (
        gtk_builder_get_object (builder, "db_context_menu"));

    GtkWidget *popover = gtk_popover_menu_new_from_model (menu_model);
    gtk_widget_set_parent (popover, self->database_list);
    GdkRectangle rect = { (int)x, (int)y, 1, 1 };
    gtk_popover_set_pointing_to (GTK_POPOVER (popover), &rect);
    g_signal_connect (popover, "closed", G_CALLBACK (on_popover_closed), NULL);
    gtk_popover_popup (GTK_POPOVER (popover));

    g_object_unref (builder);
}

static void
on_rename_dialog_response (AdwAlertDialog  *dialog,
                           const gchar     *response,
                           OTPClientWindow *self)
{
    if (g_strcmp0 (response, "rename") != 0)
        return;

    gint index = otpclient_window_get_selected_db_index (self);
    if (index < 0)
        return;

    GtkWidget *entry = g_object_get_data (G_OBJECT (dialog), "name-entry");
    const gchar *new_name = gtk_editable_get_text (GTK_EDITABLE (entry));
    if (new_name != NULL && new_name[0] != '\0')
        gui_misc_rename_db_in_list (self->db_store, (guint)index, new_name);
}

static void
action_rename_db (GtkWidget  *widget,
                  const char *action_name,
                  GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    gint index = otpclient_window_get_selected_db_index (self);
    if (index < 0)
        return;

    g_autoptr (DatabaseEntry) entry = g_list_model_get_item (
        G_LIST_MODEL (self->db_store), (guint)index);
    if (entry == NULL)
        return;

    AdwAlertDialog *dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new (
        _("Rename Database"), NULL));
    adw_alert_dialog_add_responses (dialog, "cancel", _("Cancel"),
                                    "rename", _("Rename"), NULL);
    adw_alert_dialog_set_default_response (dialog, "rename");
    adw_alert_dialog_set_close_response (dialog, "cancel");
    adw_alert_dialog_set_response_appearance (dialog, "rename",
                                              ADW_RESPONSE_SUGGESTED);

    GtkWidget *name_entry = gtk_entry_new ();
    gtk_editable_set_text (GTK_EDITABLE (name_entry),
                           database_entry_get_name (entry));
    adw_alert_dialog_set_extra_child (dialog, name_entry);
    g_object_set_data (G_OBJECT (dialog), "name-entry", name_entry);

    g_signal_connect (dialog, "response",
                      G_CALLBACK (on_rename_dialog_response), self);
    adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

static void
action_set_primary_db (GtkWidget  *widget,
                       const char *action_name,
                       GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    gint index = otpclient_window_get_selected_db_index (self);
    if (index < 0)
        return;

    g_autoptr (DatabaseEntry) entry = g_list_model_get_item (
        G_LIST_MODEL (self->db_store), (guint)index);
    if (entry == NULL)
        return;

    gui_misc_save_db_path_to_cfg (database_entry_get_path (entry));
    sync_primary_flags (self);
}

static void
on_remove_dialog_response (AdwAlertDialog  *dialog,
                           const gchar     *response,
                           OTPClientWindow *self)
{
    (void) dialog;

    if (g_strcmp0 (response, "remove") != 0)
        return;

    gint index = otpclient_window_get_selected_db_index (self);
    if (index < 0)
        return;

    g_autoptr (DatabaseEntry) entry = g_list_model_get_item (
        G_LIST_MODEL (self->db_store), (guint)index);
    if (entry == NULL)
        return;

    /* Check if this was the primary database */
    g_autofree gchar *primary_path = gui_misc_get_db_path_from_cfg ();
    gboolean was_primary = (g_strcmp0 (database_entry_get_path (entry),
                                      primary_path) == 0);

    gui_misc_remove_db_from_list (self->db_store, (guint)index);

    /* If it was primary, set the first remaining entry as primary */
    if (was_primary) {
        guint n = g_list_model_get_n_items (G_LIST_MODEL (self->db_store));
        if (n > 0) {
            g_autoptr (DatabaseEntry) first = g_list_model_get_item (
                G_LIST_MODEL (self->db_store), 0);
            gui_misc_save_db_path_to_cfg (database_entry_get_path (first));
        } else {
            gui_misc_save_db_path_to_cfg ("");
        }
        sync_primary_flags (self);
    }

    /* If the removed DB was the currently active one, check via the app */
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app != NULL) {
        DatabaseData *db_data = otpclient_application_get_db_data (app);
        if (db_data != NULL &&
            g_strcmp0 (db_data->db_path, database_entry_get_path (entry)) == 0) {
            otpclient_window_stop_otp_timer (self);
            g_list_store_remove_all (self->otp_store);
        }
    }
}

static void
action_remove_db (GtkWidget  *widget,
                  const char *action_name,
                  GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    gint index = otpclient_window_get_selected_db_index (self);
    if (index < 0)
        return;

    g_autoptr (DatabaseEntry) entry = g_list_model_get_item (
        G_LIST_MODEL (self->db_store), (guint)index);
    if (entry == NULL)
        return;

    g_autofree gchar *body = g_strdup_printf (
        _("Remove \"%s\" from the database list?\nThe database file will not be deleted."),
        database_entry_get_name (entry));

    AdwAlertDialog *dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new (
        _("Remove Database"), body));
    adw_alert_dialog_add_responses (dialog, "cancel", _("Cancel"),
                                    "remove", _("Remove"), NULL);
    adw_alert_dialog_set_default_response (dialog, "cancel");
    adw_alert_dialog_set_close_response (dialog, "cancel");
    adw_alert_dialog_set_response_appearance (dialog, "remove",
                                              ADW_RESPONSE_DESTRUCTIVE);

    g_signal_connect (dialog, "response",
                      G_CALLBACK (on_remove_dialog_response), self);
    adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

static void
on_db_entry_name_changed (DatabaseEntry *entry,
                          GParamSpec    *pspec,
                          AdwActionRow  *row)
{
    (void) pspec;
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row),
                                   database_entry_get_name (entry));
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
    {
        self->settings = g_settings_new ("com.github.paolostivanin.OTPClient");

        /* Restore saved window size */
        gint width = g_settings_get_int (self->settings, "window-width");
        gint height = g_settings_get_int (self->settings, "window-height");
        if (width > 0 && height > 0)
            gtk_window_set_default_size (GTK_WINDOW (self), width, height);

        gboolean show_sidebar = g_settings_get_boolean (self->settings, "show-sidebar");
        adw_overlay_split_view_set_show_sidebar (ADW_OVERLAY_SPLIT_VIEW (self->split_view), show_sidebar);
    }
    else
    {
        g_warning ("Settings schema 'com.github.paolostivanin.OTPClient' is not installed.");
    }

    setup_otp_view (self);
    setup_database_list (self);
    update_empty_state (self);

    /* Initialize group dropdown with default model */
    self->group_list_model = gtk_string_list_new (NULL);
    gtk_string_list_append (self->group_list_model, _("All"));
    gtk_string_list_append (self->group_list_model, _("Ungrouped"));
    gtk_drop_down_set_model (GTK_DROP_DOWN (self->group_dropdown),
                             G_LIST_MODEL (self->group_list_model));
    g_signal_connect (self->group_dropdown, "notify::selected",
                      G_CALLBACK (on_group_dropdown_changed), self);
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->sidebar_toggle_button),
                                  adw_overlay_split_view_get_show_sidebar (ADW_OVERLAY_SPLIT_VIEW (self->split_view)));

    g_signal_connect (self, "close-request", G_CALLBACK (on_close_request), self);
    g_signal_connect (self->split_view, "notify::show-sidebar", G_CALLBACK (split_view_sidebar_changed), self);
    g_signal_connect (self->sidebar_toggle_button, "clicked", G_CALLBACK (sidebar_toggle_clicked), self);
    g_signal_connect (self->otp_selection, "notify::selected", G_CALLBACK (on_otp_selection_changed), self);
    g_signal_connect (self->search_entry, "activate", G_CALLBACK (search_entry_activate), self);
g_signal_connect (self->lock_button, "clicked", G_CALLBACK (lock_button_clicked), self);
    g_signal_connect (self->new_db_button, "clicked", G_CALLBACK (new_db_button_clicked), self);
    g_signal_connect (self->open_db_button, "clicked", G_CALLBACK (open_db_button_clicked), self);

    setup_dnd (self);

    /* Right-click context menu on token list */
    GtkGesture *right_click = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (right_click), GDK_BUTTON_SECONDARY);
    g_signal_connect (right_click, "pressed", G_CALLBACK (on_token_right_click), self);
    gtk_widget_add_controller (self->otp_list, GTK_EVENT_CONTROLLER (right_click));

    /* Right-click context menu on database sidebar */
    GtkGesture *db_right_click = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (db_right_click), GDK_BUTTON_SECONDARY);
    g_signal_connect (db_right_click, "pressed", G_CALLBACK (on_db_right_click), self);
    gtk_widget_add_controller (self->database_list, GTK_EVENT_CONTROLLER (db_right_click));

    /* Inactivity reset: any key press or mouse click resets the auto-lock timer */
    GtkEventController *key_ctrl = gtk_event_controller_key_new ();
    gtk_event_controller_set_propagation_phase (key_ctrl, GTK_PHASE_CAPTURE);
    g_signal_connect (key_ctrl, "key-pressed", G_CALLBACK (on_key_pressed_inactivity), self);
    gtk_widget_add_controller (GTK_WIDGET (self), key_ctrl);

    GtkGesture *click_inactivity = gtk_gesture_click_new ();
    gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click_inactivity), GTK_PHASE_CAPTURE);
    g_signal_connect (click_inactivity, "pressed", G_CALLBACK (on_click_inactivity), self);
    gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click_inactivity));
}

static void
otpclient_window_class_init (OTPClientWindowClass *klass)
{
    G_OBJECT_CLASS(klass)->dispose = otpclient_window_dispose;

    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    gtk_widget_class_set_template_from_resource (widget_class, "/com/github/paolostivanin/OTPClient/ui/window.ui");
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, toast_overlay);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, add_button);
gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, search_bar);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, search_entry);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, lock_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, settings_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, sidebar_toggle_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, split_view);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, database_list);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, new_db_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, open_db_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, otp_list);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, content_stack);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, group_dropdown);

    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_q, GDK_CONTROL_MASK, "window.close", NULL);

    gtk_widget_class_install_action (widget_class, "window.search", NULL, (GtkWidgetActionActivateFunc)search_func);
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_f, GDK_CONTROL_MASK, "window.search", NULL);

    gtk_widget_class_install_action (widget_class, "win.hide-and-unselect", NULL, action_hide_and_unselect);
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_h, GDK_ALT_MASK, "win.hide-and-unselect", NULL);

    /* Token actions */
    gtk_widget_class_install_action (widget_class, "win.add-manual", NULL, action_add_manual);
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_n, GDK_CONTROL_MASK, "win.add-manual", NULL);
    gtk_widget_class_install_action (widget_class, "win.add-qr-file", NULL, action_add_qr_file);
    gtk_widget_class_install_action (widget_class, "win.add-qr-webcam", NULL, action_add_qr_webcam);
    gtk_widget_class_install_action (widget_class, "win.import", NULL, action_import);
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_i, GDK_CONTROL_MASK, "win.import", NULL);
    gtk_widget_class_install_action (widget_class, "win.export", NULL, action_export);
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_e, GDK_CONTROL_MASK, "win.export", NULL);
    gtk_widget_class_install_action (widget_class, "win.settings", NULL, action_settings);
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_comma, GDK_CONTROL_MASK, "win.settings", NULL);
    gtk_widget_class_install_action (widget_class, "win.edit-token", NULL, action_edit_token);
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_F2, 0, "win.edit-token", NULL);
    gtk_widget_class_install_action (widget_class, "win.delete-token", NULL, action_delete_token);
    gtk_widget_class_install_action (widget_class, "win.show-qr", NULL, action_show_qr);
    gtk_widget_class_install_action (widget_class, "win.move-token", NULL, action_move_token);
    gtk_widget_class_install_action (widget_class, "win.set-group", "s", action_set_group);
    gtk_widget_class_install_action (widget_class, "win.new-group", NULL, action_new_group);
    gtk_widget_class_install_action (widget_class, "win.remove-from-group", NULL, action_remove_from_group);
    gtk_widget_class_install_action (widget_class, "win.rename-db", NULL, action_rename_db);
    gtk_widget_class_install_action (widget_class, "win.set-primary-db", NULL, action_set_primary_db);
    gtk_widget_class_install_action (widget_class, "win.remove-db", NULL, action_remove_db);

    gtk_widget_class_bind_template_callback (klass, search_text_changed);
}

GtkWidget *
otpclient_window_new (OTPClientApplication *application)
{
    return g_object_new (OTPCLIENT_TYPE_WINDOW, "application", application, NULL);
}
