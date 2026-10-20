#define _DEFAULT_SOURCE
#include <errno.h>
#include <glib/gi18n.h>
#include <adwaita.h>
#include <stdlib.h>
#include <string.h>
#include "otpclient-application.h"
#include "otpclient-window.h"
#include "otpclient-window-private.h"

static void close_webcam_progress (OTPClientWindow *self);
#include "otp-entry.h"
#include "database-sidebar.h"
#include "db-common.h"
#include "qrcode-parser.h"
#include "google-migration.h"
#include "webcam-scanner.h"
#include "parse-uri.h"
#include "dialogs/manual-add-dialog.h"
#include "dialogs/import-dialog.h"
#include "dialogs/export-dialog.h"
#include "dialogs/edit-token-dialog.h"
#include "dialogs/qr-display-dialog.h"
#include "dialogs/settings-dialog.h"
#include "dialogs/password-dialog.h"
#include "dialogs/sensitive-dialog.h"
#include "gui-misc.h"
#include "lock-app.h"
#include "secret-schema.h"
#include "gsettings-common.h"
#include "common.h"
#include "otp-validation.h"

G_DEFINE_FINAL_TYPE (OTPClientWindow, otpclient_window, ADW_TYPE_APPLICATION_WINDOW)

static void trigger_otp_action_for_entry (OTPClientWindow *self, OTPEntry *entry);
static void add_action_column (GtkColumnView *view);
static void refresh_otp_value_cells (OTPClientWindow *self);
static void on_hide_otps_changed   (GObject *gobject, GParamSpec *pspec, gpointer user_data);
static void copy_otp_to_clipboard_and_notify (OTPClientWindow *self, OTPEntry *entry);
static void refresh_search_cache (OTPClientWindow *self);
static void update_empty_state (OTPClientWindow *self);
static void update_quarantine_banner (OTPClientWindow *self);
static void update_bulk_selection_count (OTPClientWindow *self);

static inline gboolean
window_is_locked (OTPClientWindow *self)
{
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    return app != NULL && otpclient_application_get_app_locked (app);
}

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

/* One shared provider for every otp-validity level bar. Per-row state is a
 * CSS class, not a per-row provider: rows are recycled GtkListItems, and
 * display-wide providers registered at APPLICATION priority raced each other,
 * so the winning color was registration (bind/recycle) order rather than each
 * row's remaining time - warning colors bled onto all rows or were masked on
 * all rows, flipping as rows recycled. The shared CSS is reloaded only when
 * the configured colors change. */
static GtkCssProvider *otp_validity_provider = NULL;
static gchar *otp_validity_provider_colors = NULL;

static void
validity_ensure_shared_css (const gchar *normal_color,
                            const gchar *warning_color)
{
    if (otp_validity_provider == NULL)
    {
        otp_validity_provider = gtk_css_provider_new ();
        gtk_style_context_add_provider_for_display (
            gdk_display_get_default (),
            GTK_STYLE_PROVIDER (otp_validity_provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

    g_autofree gchar *key = g_strdup_printf ("%s|%s",
                                             normal_color != NULL ? normal_color : "",
                                             warning_color != NULL ? warning_color : "");
    if (g_strcmp0 (key, otp_validity_provider_colors) != 0)
    {
        g_autofree gchar *css = g_strdup_printf (
            "levelbar.otp-validity.otp-validity-normal trough block.filled { background-color: %s; }"
            "levelbar.otp-validity.otp-validity-warning trough block.filled { background-color: %s; }",
            normal_color != NULL ? normal_color : "green",
            warning_color != NULL ? warning_color : "red");
        gtk_css_provider_load_from_string (otp_validity_provider, css);
        g_free (otp_validity_provider_colors);
        otp_validity_provider_colors = g_steal_pointer (&key);
    }
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

            /* Apply color based on remaining time by toggling a class; the
             * colors themselves live in the shared provider above. add/remove
             * are no-ops when the class is already (un)set. */
            const gchar *normal_color = otpclient_application_get_validity_color (app);
            const gchar *warning_color = otpclient_application_get_validity_warning_color (app);
            validity_ensure_shared_css (normal_color, warning_color);

            if (widgets->remaining <= widgets->period / 4)
            {
                gtk_widget_add_css_class (widgets->level_bar, "otp-validity-warning");
                gtk_widget_remove_css_class (widgets->level_bar, "otp-validity-normal");
            }
            else
            {
                gtk_widget_add_css_class (widgets->level_bar, "otp-validity-normal");
                gtk_widget_remove_css_class (widgets->level_bar, "otp-validity-warning");
            }

            gtk_widget_set_visible (widgets->level_bar, TRUE);
        }
    }
}

/* The countdown tracks the visibility of the code it counts down, and nothing
 * else. It is shown whenever the value cell is showing a TOTP: either "Hide
 * OTPs by default" is off, or it is on and this row is currently revealed.
 *
 * Selection deliberately does not enter into it. It used to: before selection
 * became inert, selecting a row was what revealed and copied its code, so
 * "selected" and "showing a code" were the same state. They no longer are, and
 * gating on selection left a countdown missing next to a perfectly visible
 * code - most obviously after the row's own Action button revealed one, since
 * the button claims the gesture and the row never gets selected.
 *
 * HOTP is excluded. Its codes are not on a wallclock schedule, they last until
 * consumed, so the period the bar would animate is meaningless. */
static gboolean
validity_should_show (GtkListItem *list_item)
{
    OTPEntry *entry = OTP_ENTRY (gtk_list_item_get_item (list_item));
    if (entry == NULL)
        return FALSE;

    /* g_ascii_strcasecmp is not NULL-safe and a NULL type must not read as
     * HOTP - same guard as trigger_otp_action_for_entry. */
    const gchar *type = otp_entry_get_otp_type (entry);
    if (type != NULL && g_ascii_strcasecmp (type, "HOTP") == 0)
        return FALSE;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (g_application_get_default ());
    gboolean hide = app != NULL && otpclient_application_get_hide_otps (app);

    return !hide || otp_entry_get_revealed (entry);
}

static gboolean
validity_tick (gpointer data)
{
    GtkListItem *list_item = GTK_LIST_ITEM (data);

    if (!GTK_IS_LIST_ITEM (list_item))
        return G_SOURCE_REMOVE;

    /* Looked up before the bail-outs below, because every return that removes
     * the source has to clear the id it is stored under first. A stale
     * timeout_id is a live g_source_remove aimed at whichever source GLib has
     * since handed that number to, clipboard_clear_timer_id being the nearest
     * candidate. */
    ValidityWidgets *widgets = g_object_get_data (G_OBJECT (list_item), "validity-widgets");
    if (widgets == NULL)
        return G_SOURCE_REMOVE;

    GtkWidget *child = gtk_list_item_get_child (list_item);
    if (child == NULL || !GTK_IS_WIDGET (child))
    {
        widgets->timeout_id = 0;
        return G_SOURCE_REMOVE;
    }

    if (!validity_should_show (list_item))
    {
        gtk_widget_set_visible (widgets->box, FALSE);
        widgets->timeout_id = 0;
        return G_SOURCE_REMOVE;
    }

    /* Recompute from wallclock instead of decrementing a counter and
     * giving up at 0. At rollover (now % period == 0), `remaining`
     * becomes `period` automatically and the bar refills the moment the
     * OTP rotates - otp_refresh_tick handles updating the OTP value in
     * parallel. */
    if (widgets->period > 0)
    {
        gint64 now = g_get_real_time () / G_USEC_PER_SEC;
        widgets->remaining = widgets->period - (guint32) (now % widgets->period);
    }
    validity_update_display (widgets);

    return G_SOURCE_CONTINUE;
}

/* Decide whether the validity bar/seconds should be visible right now and
 * (re)arm or stop the per-row timer accordingly. */
static void
validity_apply_visibility (GtkListItem *list_item)
{
    if (!GTK_IS_LIST_ITEM (list_item))
        return;

    ValidityWidgets *widgets = g_object_get_data (G_OBJECT (list_item), "validity-widgets");
    if (widgets == NULL)
        return;

    OTPEntry *entry = OTP_ENTRY (gtk_list_item_get_item (list_item));
    gboolean show_bar = validity_should_show (list_item);

    if (widgets->timeout_id != 0)
    {
        g_source_remove (widgets->timeout_id);
        widgets->timeout_id = 0;
    }

    if (show_bar)
    {
        guint32 period = otp_entry_get_period (entry);
        if (period == 0)
            period = 30;
        gint64 now = g_get_real_time () / G_USEC_PER_SEC;
        widgets->period = period;
        widgets->remaining = period - (guint32) (now % period);
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
on_validity_entry_changed (OTPEntry   *entry,
                           GParamSpec *pspec,
                           gpointer    user_data)
{
    (void) entry;
    (void) pspec;
    validity_apply_visibility (GTK_LIST_ITEM (user_data));
}

static void
validity_list_item_unbind (GtkSignalListItemFactory *factory,
                           GtkListItem              *list_item,
                           gpointer                  user_data)
{
    (void) factory;
    (void) user_data;

    OTPEntry *entry = g_object_get_data (G_OBJECT (list_item), "validity-bound-entry");
    if (entry != NULL)
        g_signal_handlers_disconnect_by_data (entry, list_item);
    g_object_set_data (G_OBJECT (list_item), "validity-bound-entry", NULL);

    ValidityWidgets *widgets = g_object_get_data (G_OBJECT (list_item), "validity-widgets");

    if (widgets == NULL)
        return;

    if (widgets->timeout_id != 0)
    {
        g_source_remove (widgets->timeout_id);
        widgets->timeout_id = 0;
    }

    /* Hide the whole cell, not just the label: a recycled row would otherwise
     * carry the previous entry's level bar until bind runs again. */
    gtk_widget_set_visible (widgets->box, FALSE);
}

/* Render the value-column label for a single entry, honoring both the
 * "Show Next OTP" toggle and the "Hide OTPs by default" mask. Called from
 * the factory bind AND from notify::revealed / notify::otp-value handlers
 * so a row can update in place without a full rebind. */
static void
render_otp_value_label (GtkWidget *label,
                        OTPEntry  *entry)
{
    if (label == NULL || !GTK_IS_LABEL (label) || entry == NULL)
        return;

    /* Use g_application_get_default() rather than walking up from the label -
     * during the very first row bind the label may not be parented yet, so
     * gtk_widget_get_root returns NULL, app falls through to NULL, and `hide`
     * defaults to FALSE -> OTPs flash unmasked on startup. The app exists from
     * GApplication startup, well before any column row binds. Same pattern as
     * validity_update_display. */
    OTPClientApplication *app = OTPCLIENT_APPLICATION (g_application_get_default ());
    gboolean hide = app != NULL && otpclient_application_get_hide_otps (app);
    gboolean masked = hide && !otp_entry_get_revealed (entry);

    /* When masked, the cell is left visually empty rather than carrying a word
     * or a bullet placeholder - the row's other columns (account / issuer /
     * validity) already make it obvious the entry exists, the Action column
     * says what to do about it, and a blank value gives away no digit count.
     * A placeholder would also not disambiguate anything: an HOTP that has
     * never been generated is masked too, so it read the same either way. */
    if (masked)
    {
        gtk_label_set_use_markup (GTK_LABEL (label), FALSE);
        gtk_label_set_text (GTK_LABEL (label), "");
        return;
    }

    const gchar *current = otp_entry_get_otp_value (entry);
    if (current == NULL || *current == '\0')
    {
        gtk_label_set_use_markup (GTK_LABEL (label), FALSE);
        gtk_label_set_text (GTK_LABEL (label), "");
        return;
    }

    if (app != NULL && otpclient_application_get_show_next_otp (app))
    {
        /* cotp hands back a plain malloc'd code and every other consumer
         * releases it through sensitive_free so it leaves no cleartext behind
         * (otp-entry.c:487, :492, :527). The combined string holds both the
         * current and the next code, so it gets the same treatment on the
         * GLib side. gtk_label_set_text has copied what it needs by then. */
        gchar *next = otp_entry_get_next_otp (entry);
        if (next != NULL)
        {
            gchar *combined = g_strdup_printf ("%s  [%s]", current, next);
            sensitive_free (next);
            gtk_label_set_use_markup (GTK_LABEL (label), FALSE);
            gtk_label_set_text (GTK_LABEL (label), combined);
            sensitive_g_free (combined);
            return;
        }
    }

    gtk_label_set_use_markup (GTK_LABEL (label), FALSE);
    gtk_label_set_text (GTK_LABEL (label), current);
}

static void
on_otp_entry_notify_refresh (OTPEntry   *entry,
                             GParamSpec *pspec,
                             gpointer    user_data)
{
    (void) pspec;
    render_otp_value_label (GTK_WIDGET (user_data), entry);
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
    gtk_label_set_ellipsize (GTK_LABEL (label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars (GTK_LABEL (label), 28);
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
            /* For cross-DB entries we never computed the OTP upfront; do it
             * the first time the row is bound for display. The result is
             * cached on the entry so subsequent rebinds are free. */
            if (otp_entry_get_db_name (entry) != NULL)
            {
                const gchar *cached = otp_entry_get_otp_value (entry);
                if (cached == NULL || cached[0] == '\0')
                    otp_entry_update_otp (entry);
            }
            /* Drop any signal connections from a prior entry - labels are
             * recycled across rows as the user scrolls. Then connect to the
             * new entry so reveal/hide and HOTP/TOTP updates re-render in
             * place. */
            {
                OTPEntry *prev = g_object_get_data (G_OBJECT (label), "value-bound-entry");
                if (prev != NULL && prev != entry)
                    g_signal_handlers_disconnect_by_data (prev, label);
                if (prev != entry)
                {
                    g_signal_connect (entry, "notify::revealed",
                                      G_CALLBACK (on_otp_entry_notify_refresh), label);
                    g_signal_connect (entry, "notify::otp-value",
                                      G_CALLBACK (on_otp_entry_notify_refresh), label);
                    g_object_set_data (G_OBJECT (label), "value-bound-entry", entry);
                }
            }
            render_otp_value_label (label, entry);
            return;
        default:
            break;
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
        gtk_widget_set_tooltip_text (label, text && text[0] != '\0' ? text : NULL);
        return;
    }

    gtk_label_set_use_markup (GTK_LABEL (label), FALSE);
    gtk_label_set_text (GTK_LABEL (label), text ? text : "");
    gtk_widget_set_tooltip_text (label, text && text[0] != '\0' ? text : NULL);
}

static void
otp_text_column_unbind (GtkSignalListItemFactory *factory,
                        GtkListItem              *list_item,
                        gpointer                  user_data)
{
    (void) factory;
    OTPColumn column = GPOINTER_TO_INT (user_data);

    GtkWidget *label = gtk_list_item_get_child (list_item);
    if (label == NULL)
        return;

    /* Every column sets this on bind, so every column drops it on unbind. The
     * widget is recycled and outlives the binding, and the pointer it holds is
     * raw and unreffed, so a cell left carrying it names an entry that may be
     * gone by the time pick_entry_at walks up to read it. */
    g_object_set_data (G_OBJECT (label), "otp-entry", NULL);

    if (column != OTP_COLUMN_VALUE)
        return;

    OTPEntry *entry = g_object_get_data (G_OBJECT (label), "value-bound-entry");
    if (entry != NULL)
        g_signal_handlers_disconnect_by_data (entry, label);
    g_object_set_data (G_OBJECT (label), "value-bound-entry", NULL);
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
    /* Selector hook for the shared validity CSS (see validity_update_display);
     * the per-row normal/warning class is toggled on top of this. */
    gtk_widget_add_css_class (level_bar, "otp-validity");
    gtk_box_append (GTK_BOX (box), level_bar);

    gtk_list_item_set_child (list_item, box);

    widgets->box = box;
    widgets->label = label;
    widgets->level_bar = level_bar;
    widgets->remaining = 30;
    widgets->period = 30;
    g_object_set_data_full (G_OBJECT (list_item), "validity-widgets", widgets, (GDestroyNotify) validity_widgets_free);
}

static void
otp_validity_column_bind (GtkSignalListItemFactory *factory,
                          GtkListItem              *list_item,
                          gpointer                  user_data)
{
    (void) factory;
    (void) user_data;

    /* Track the entry's reveal state so the bar can hide along with the
     * OTP when the auto-hide timer fires, and notify::otp-value so it can
     * appear or disappear the moment "Hide OTPs by default" is toggled -
     * refresh_otp_value_cells re-emits that property for every entry, and
     * the value cell already rides on it for the same reason. List items are
     * recycled across scrolling, so disconnect from any prior entry first. */
    OTPEntry *entry = OTP_ENTRY (gtk_list_item_get_item (list_item));
    OTPEntry *prev = g_object_get_data (G_OBJECT (list_item), "validity-bound-entry");
    if (prev != NULL && prev != entry)
        g_signal_handlers_disconnect_by_data (prev, list_item);
    if (entry != NULL && prev != entry)
    {
        g_signal_connect (entry, "notify::revealed",
                          G_CALLBACK (on_validity_entry_changed), list_item);
        g_signal_connect (entry, "notify::otp-value",
                          G_CALLBACK (on_validity_entry_changed), list_item);
        g_object_set_data (G_OBJECT (list_item), "validity-bound-entry", entry);
    }

    validity_apply_visibility (list_item);
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
    g_signal_connect (factory, "unbind", G_CALLBACK (otp_text_column_unbind), GINT_TO_POINTER (column));

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
    g_object_unref (view_column);
}

static void
bulk_selection_toggled (GtkCheckButton *button, OTPClientWindow *self)
{
    OTPEntry *entry = g_object_get_data (G_OBJECT (button), "otp-entry");
    if (entry == NULL || otp_entry_get_db_name (entry) != NULL)
        return;
    if (gtk_check_button_get_active (button))
        g_hash_table_add (self->bulk_selected_entries, g_object_ref (entry));
    else
        g_hash_table_remove (self->bulk_selected_entries, entry);
    update_bulk_selection_count (self);
}

static void
bulk_selection_column_setup (GtkSignalListItemFactory *factory,
                             GtkListItem              *item,
                             gpointer                  user_data)
{
    (void) factory;
    GtkWidget *check = gtk_check_button_new ();
    gtk_widget_set_halign (check, GTK_ALIGN_CENTER);
    g_signal_connect (check, "toggled", G_CALLBACK (bulk_selection_toggled), user_data);
    gtk_list_item_set_child (item, check);
}

static void
bulk_selection_column_bind (GtkSignalListItemFactory *factory,
                            GtkListItem              *item,
                            gpointer                  user_data)
{
    (void) factory;
    OTPClientWindow *self = user_data;
    OTPEntry *entry = gtk_list_item_get_item (item);
    GtkWidget *check = gtk_list_item_get_child (item);
    if (entry == NULL || check == NULL)
        return;
    g_object_set_data (G_OBJECT (check), "otp-entry", entry);
    g_signal_handlers_block_by_func (check, bulk_selection_toggled, self);
    gtk_check_button_set_active (GTK_CHECK_BUTTON (check),
        g_hash_table_contains (self->bulk_selected_entries, entry));
    gtk_widget_set_visible (check, self->bulk_mode);
    gtk_widget_set_sensitive (check, otp_entry_get_db_name (entry) == NULL);
    gtk_widget_set_tooltip_text (check, otp_entry_get_db_name (entry) == NULL
        ? _("Select token") : _("Tokens from other databases are read-only."));
    g_signal_handlers_unblock_by_func (check, bulk_selection_toggled, self);
}

static void
bulk_selection_column_unbind (GtkSignalListItemFactory *factory,
                              GtkListItem              *item,
                              gpointer                  user_data)
{
    (void) factory;
    (void) user_data;
    GtkWidget *check = gtk_list_item_get_child (item);
    if (check != NULL)
        g_object_set_data (G_OBJECT (check), "otp-entry", NULL);
}

static void
add_bulk_selection_column (OTPClientWindow *self)
{
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
    self->bulk_selection_column = gtk_column_view_column_new ("", factory);
    gtk_column_view_column_set_fixed_width (self->bulk_selection_column, 42);
    g_signal_connect (factory, "setup", G_CALLBACK (bulk_selection_column_setup), self);
    g_signal_connect (factory, "bind", G_CALLBACK (bulk_selection_column_bind), self);
    g_signal_connect (factory, "unbind", G_CALLBACK (bulk_selection_column_unbind), self);
    gtk_column_view_append_column (GTK_COLUMN_VIEW (self->otp_list),
                                   self->bulk_selection_column);
    g_object_unref (self->bulk_selection_column);
}

static void
update_bulk_selection_count (OTPClientWindow *self)
{
    guint count = self->bulk_selected_entries != NULL
        ? g_hash_table_size (self->bulk_selected_entries) : 0;
    if (self->bulk_count_label != NULL) {
        g_autofree gchar *text = g_strdup_printf (ngettext ("%u selected", "%u selected", count), count);
        gtk_label_set_text (GTK_LABEL (self->bulk_count_label), text);
    }
    const gchar * const actions[] = { "win.bulk-set-group", "win.bulk-export", "win.bulk-delete" };
    for (guint i = 0; i < G_N_ELEMENTS (actions); i++)
        gtk_widget_action_set_enabled (GTK_WIDGET (self), actions[i], self->bulk_mode && count > 0);
}

static void
bulk_mode_toggled (GtkToggleButton *button, OTPClientWindow *self)
{
    self->bulk_mode = gtk_toggle_button_get_active (button);
    gtk_widget_set_visible (self->bulk_toolbar, self->bulk_mode);
    if (!self->bulk_mode && self->bulk_selected_entries != NULL)
        g_hash_table_remove_all (self->bulk_selected_entries);
    update_bulk_selection_count (self);
    gtk_button_set_label (GTK_BUTTON (button), self->bulk_mode ? _("Done") : _("Select"));
    gtk_widget_set_tooltip_text (GTK_WIDGET (button), self->bulk_mode
        ? _("Finish selecting tokens") : _("Select multiple tokens"));
}

static GArray *
bulk_selected_indices (OTPClientWindow *self, json_t **out_tokens)
{
    GArray *indices = g_array_new (FALSE, FALSE, sizeof (guint));
    json_t *tokens = out_tokens != NULL ? json_array () : NULL;
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    DatabaseData *db = app != NULL ? otpclient_application_get_db_data (app) : NULL;
    if (db == NULL || db->in_memory_json_data == NULL) {
        if (out_tokens != NULL)
            *out_tokens = tokens;
        return indices;
    }
    guint count = g_list_model_get_n_items (G_LIST_MODEL (self->otp_store));
    for (guint i = 0; i < count; i++) {
        g_autoptr (OTPEntry) entry = g_list_model_get_item (G_LIST_MODEL (self->otp_store), i);
        if (!g_hash_table_contains (self->bulk_selected_entries, entry))
            continue;
        g_array_append_val (indices, i);
        if (tokens != NULL) {
            json_t *token = json_array_get (db->in_memory_json_data, i);
            if (token != NULL)
                json_array_append (tokens, token);
        }
    }
    if (out_tokens != NULL)
        *out_tokens = tokens;
    return indices;
}

GArray *
otpclient_window_get_bulk_selected_indices (OTPClientWindow *self,
                                            json_t **out_tokens)
{
    return bulk_selected_indices (self, out_tokens);
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
    g_object_unref (view_column);
}

/* Refreshes one TOTP entry when its step has changed since we last rendered
 * it. Tracking the last rendered step (rather than requiring an exact
 * `now % period == 0` tick) means a delayed timer, a missed tick across
 * suspend/resume, or a system clock jump still lands on a refresh instead of
 * showing a stale code for up to a full period. */
static void
refresh_totp_entry (OTPClientWindow *self,
                    OTPEntry        *entry,
                    gboolean         show_next,
                    gint64           now)
{
    const gchar *type = otp_entry_get_otp_type (entry);
    if (type == NULL || g_ascii_strcasecmp (type, "TOTP") != 0)
        return;

    guint32 period = otp_entry_get_period (entry);
    if (period == 0)
        return;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    g_autoptr (OTPEntry) clip_owner = g_weak_ref_get (&self->clipboard_owner_entry);
    gboolean owns_clipboard = clip_owner == entry;
    /* Cross-database entries are cached lazily as search results bind. Do not
     * turn the periodic refresh into an eager generator for cached rows that
     * were never shown; an explicit reveal/clipboard owner still refreshes. */
    const gchar *cached_value = otp_entry_get_otp_value (entry);
    if (otp_entry_get_db_name (entry) != NULL &&
        (cached_value == NULL || cached_value[0] == '\0') &&
        !otp_entry_get_revealed (entry) && !owns_clipboard)
        return;
    if (app != NULL && otpclient_application_get_hide_otps (app) &&
        !otp_entry_get_revealed (entry) && !owns_clipboard)
        return;

    gint64 step = now / (gint64) period;
    /* The generation step is recorded by otp_entry_update_otp(), including on
     * explicit activation. Comparing against it (instead of against a value we
     * only update during ticks) stops a delayed tick from hiding a code that
     * was just generated, and stops a code from surviving its first boundary
     * because the tick that should have recorded it never ran. */
    gint64 last_step = otp_entry_get_last_rendered_step (entry);
    gboolean rotated = (last_step != 0 && last_step != step);

    if (last_step == step)
        return;

    otp_entry_update_otp (entry);

    /* Reveal lifetime is tied to the validity window: if this entry was
     * revealed when the period rolled over, either auto-roll (re-copy +
     * re-notify the new value if this entry still owns the clipboard and the
     * user opted into "show next OTP", and only once per reveal session) or
     * hide it. Only a genuine rotation (not the first time we see the entry)
     * triggers this. */
    if (!rotated || !otp_entry_get_revealed (entry))
        return;

    if (show_next && owns_clipboard && !otp_entry_get_roll_consumed (entry))
    {
        copy_otp_to_clipboard_and_notify (self, entry);
        otp_entry_mark_roll_consumed (entry);
    }
    else
    {
        if (owns_clipboard)
            g_weak_ref_set (&self->clipboard_owner_entry, NULL);
        otp_entry_set_revealed (entry, FALSE);
    }
}

static gboolean
otp_refresh_tick (gpointer user_data)
{
    OTPClientWindow *self = OTPCLIENT_WINDOW (user_data);

    if (self->otp_store == NULL)
        return G_SOURCE_REMOVE;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    gboolean show_next = (app != NULL && otpclient_application_get_show_next_otp (app));
    gint64 now = g_get_real_time () / G_USEC_PER_SEC;

    guint n = g_list_model_get_n_items (G_LIST_MODEL (self->otp_store));
    for (guint i = 0; i < n; i++)
    {
        g_autoptr (OTPEntry) entry = g_list_model_get_item (G_LIST_MODEL (self->otp_store), i);
        if (entry != NULL)
            refresh_totp_entry (self, entry, show_next, now);
    }

    /* Cross-database search results are computed lazily when bound, so they
     * need the same rotation/reveal pass or they keep showing an expired (or
     * revealed) code forever. */
    if (self->cross_db_store != NULL)
    {
        n = g_list_model_get_n_items (G_LIST_MODEL (self->cross_db_store));
        for (guint i = 0; i < n; i++)
        {
            g_autoptr (OTPEntry) entry = g_list_model_get_item (G_LIST_MODEL (self->cross_db_store), i);
            if (entry != NULL)
                refresh_totp_entry (self, entry, show_next, now);
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
    GPtrArray *loaded_db_paths;
    gchar *current_db_path;
    gint32 max_file_size;
    GWeakRef window_ref;
    guint64 generation;
} CrossDbTaskData;

typedef struct {
    GWeakRef window_ref;
} CrossDbMonitorContext;

static void cross_db_trigger_load (OTPClientWindow *self);

static void
cross_db_monitor_context_free (gpointer data,
                              GClosure *closure G_GNUC_UNUSED)
{
    CrossDbMonitorContext *ctx = data;
    g_weak_ref_clear (&ctx->window_ref);
    g_free (ctx);
}

static void
cross_db_task_data_free (gpointer data)
{
    CrossDbTaskData *td = data;
    g_weak_ref_clear (&td->window_ref);
    g_free (td->current_db_path);
    if (td->db_list != NULL)
        g_ptr_array_unref (td->db_list);
    if (td->loaded_db_paths != NULL)
        g_ptr_array_unref (td->loaded_db_paths);
    g_free (td);
}

static void
cross_db_load_thread (GTask        *task,
                      gpointer      source,
                      gpointer      task_data,
                      GCancellable *cancellable)
{
    (void) source;
    CrossDbTaskData *td = task_data;
    GListStore *result = g_list_store_new (OTP_TYPE_ENTRY);

    for (guint i = 0; i < td->db_list->len; i++)
    {
        if (g_cancellable_is_cancelled (cancellable)) {
            g_task_return_new_error (task, G_IO_ERROR, G_IO_ERROR_CANCELLED,
                                     "Cross-database load cancelled");
            g_object_unref (result);
            return;
        }

        DbListEntry *dbe = g_ptr_array_index (td->db_list, i);

        /* Skip the currently active database */
        if (g_strcmp0 (dbe->path, td->current_db_path) == 0)
            continue;

        /* Look up password from Secret Service */
        gchar *pwd = secret_password_lookup_sync (OTPCLIENT_SCHEMA, cancellable, NULL,
                                                   "string", dbe->path, NULL);
        if (pwd == NULL)
            continue;   /* No password stored for this DB -- skip it */

        DatabaseData *db_data = database_data_new (dbe->path, td->max_file_size);
        db_data->key = secure_strdup (pwd);
        secret_password_free (pwd);

        GError *err = NULL;
        load_db (db_data, &err);
        if (err != NULL || db_data->in_memory_json_data == NULL)
        {
            if (err != NULL)
                g_clear_error (&err);
            database_data_free (db_data);
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
            otp_entry_set_db_path (entry, dbe->path);
            /* OTP is computed lazily - see otp_text_column_bind /
             * trigger_otp_action_for_entry. Computing every cross-DB entry's OTP
             * upfront wastes cycles when the user only opens search to find
             * one token. */
            g_list_store_append (result, entry);
            g_object_unref (entry);
        }

        g_ptr_array_add (td->loaded_db_paths, g_strdup (dbe->path));

        database_data_free (db_data);
    }

    g_task_return_pointer (task, result, g_object_unref);
}

static void
cross_db_source_changed (GFileMonitor      *monitor,
                         GFile             *file,
                         GFile             *other_file,
                         GFileMonitorEvent  event,
                         gpointer           user_data)
{
    (void) monitor;
    (void) file;
    (void) other_file;
    (void) event;
    CrossDbMonitorContext *ctx = user_data;
    g_autoptr (OTPClientWindow) self = g_weak_ref_get (&ctx->window_ref);
    if (self == NULL || self->disposing)
        return;

    otpclient_window_invalidate_cross_db (self);
    const gchar *query = gtk_editable_get_text (GTK_EDITABLE (self->search_entry));
    if (query != NULL && query[0] != '\0')
        cross_db_trigger_load (self);
}

static void
cross_db_watch_source (OTPClientWindow *self,
                       const gchar     *path)
{
    if (self->cross_db_monitors == NULL ||
        g_hash_table_contains (self->cross_db_monitors, path))
        return;
    g_autoptr (GFile) file = g_file_new_for_path (path);
    g_autoptr (GError) err = NULL;
    GFileMonitor *monitor = g_file_monitor_file (file, G_FILE_MONITOR_WATCH_MOVES,
                                                 NULL, &err);
    if (monitor == NULL) {
        g_debug ("Could not watch cross-database source %s: %s", path, err->message);
        return;
    }
    CrossDbMonitorContext *ctx = g_new0 (CrossDbMonitorContext, 1);
    g_weak_ref_init (&ctx->window_ref, self);
    g_signal_connect_data (monitor, "changed", G_CALLBACK (cross_db_source_changed),
                           ctx, cross_db_monitor_context_free, 0);
    g_hash_table_insert (self->cross_db_monitors, g_strdup (path), monitor);
}

static void
cross_db_load_done (GObject      *source,
                    GAsyncResult *result,
                    gpointer      user_data)
{
    (void) source;

    CrossDbTaskData *td = user_data;
    g_autoptr (OTPClientWindow) self = g_weak_ref_get (&td->window_ref);
    if (self == NULL)
        return;

    if (td->generation != self->cross_db_generation || self->disposing) {
        return;
    }
    self->cross_db_loading = FALSE;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    DatabaseData *active_db = app != NULL
        ? otpclient_application_get_db_data (app) : NULL;
    if (active_db == NULL ||
        g_strcmp0 (active_db->db_path, td->current_db_path) != 0 ||
        !otpclient_application_get_use_secret_service (app))
        return;

    GError *err = NULL;
    GListStore *store = g_task_propagate_pointer (G_TASK (result), &err);
    if (err != NULL) {
        if (!g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning ("Cross-database load failed: %s", err->message);
        g_clear_error (&err);
        return;
    }
    if (store == NULL)
        return;

    g_clear_object (&self->cross_db_store);
    self->cross_db_store = store;
    self->cross_db_loaded = TRUE;
    for (guint i = 0; i < td->loaded_db_paths->len; i++)
        cross_db_watch_source (self, g_ptr_array_index (td->loaded_db_paths, i));

    /* Rebuild the flatten model with the new cross-db store */
    g_clear_object (&self->flatten_model);

    /* If there's an active search, wire up the flatten model */
    const gchar *text = gtk_editable_get_text (GTK_EDITABLE (self->search_entry));
    if (text != NULL && text[0] != '\0')
    {
        cross_db_activate (self);
        refresh_search_cache (self);
        gtk_filter_changed (GTK_FILTER (self->search_filter), GTK_FILTER_CHANGE_DIFFERENT);
        update_empty_state (self);
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
    g_clear_object (&self->cross_db_cancellable);
    self->cross_db_cancellable = g_cancellable_new ();

    CrossDbTaskData *td = g_new0 (CrossDbTaskData, 1);
    td->db_list = db_list;
    td->loaded_db_paths = g_ptr_array_new_with_free_func (g_free);
    td->current_db_path = g_strdup (db_data->db_path);
    td->max_file_size = db_data->max_file_size_from_memlock;
    td->generation = self->cross_db_generation;
    g_weak_ref_init (&td->window_ref, self);

    GTask *task = g_task_new (NULL, self->cross_db_cancellable, cross_db_load_done, td);
    g_task_set_task_data (task, td, cross_db_task_data_free);
    g_task_run_in_thread (task, cross_db_load_thread);
    g_object_unref (task);
}

void
otpclient_window_invalidate_cross_db (OTPClientWindow *self)
{
    g_return_if_fail (OTPCLIENT_IS_WINDOW (self));
    self->cross_db_generation++;
    if (self->cross_db_cancellable != NULL)
        g_cancellable_cancel (self->cross_db_cancellable);
    g_clear_object (&self->cross_db_cancellable);
    self->cross_db_loading = FALSE;
    self->cross_db_loaded = FALSE;
    if (self->cross_db_monitors != NULL)
        g_hash_table_remove_all (self->cross_db_monitors);
    cross_db_deactivate (self);
    g_clear_object (&self->flatten_model);
    if (self->cross_db_store != NULL)
        g_list_store_remove_all (self->cross_db_store);
}

void
otpclient_window_clear_undo_actions (OTPClientWindow *self)
{
    g_return_if_fail (OTPCLIENT_IS_WINDOW (self));
    otpclient_window_undo_clear (self);
    if (self->bulk_mode_button != NULL)
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->bulk_mode_button), FALSE);
    if (self->bulk_selected_entries != NULL)
        g_hash_table_remove_all (self->bulk_selected_entries);
}

/* Split a group-qualified query into its group and remaining text. Quoted names
 * may contain spaces: group:"Work Projects" alice (or #"Work Projects"). */
static gchar *
parse_search_group (const gchar  *text,
                    const gchar **remaining_text)
{
    const gchar *rest = NULL;
    if (g_str_has_prefix (text, "group:"))
        rest = text + 6;
    else if (text[0] == '#' && text[1] != '\0')
        rest = text + 1;

    if (rest == NULL) {
        *remaining_text = text;
        return NULL;
    }

    if (*rest == '"') {
        const gchar *end_quote = strchr (rest + 1, '"');
        if (end_quote != NULL) {
            gchar *group = g_strndup (rest + 1, end_quote - (rest + 1));
            rest = end_quote + 1;
            if (*rest == ' ')
                rest++;
            *remaining_text = *rest != '\0' ? rest : NULL;
            return group;
        }
    }

    const gchar *space = strchr (rest, ' ');
    gchar *group = space != NULL ? g_strndup (rest, space - rest) : g_strdup (rest);
    *remaining_text = space != NULL && space[1] != '\0' ? space + 1 : NULL;
    return group;
}

/* Re-derive search_lower / search_group_lower from the live search entry text.
 * Must be called from every code path that ends with gtk_filter_changed on the
 * search filter - otherwise the per-row filter will run against stale strings. */
static void
refresh_search_cache (OTPClientWindow *self)
{
    g_clear_pointer (&self->search_lower, g_free);
    g_clear_pointer (&self->search_group_lower, g_free);

    const gchar *text = gtk_editable_get_text (GTK_EDITABLE (self->search_entry));
    if (text == NULL || text[0] == '\0')
        return;

    const gchar *remaining_text = NULL;
    g_autofree gchar *search_group = parse_search_group (text, &remaining_text);

    if (search_group != NULL)
        self->search_group_lower = g_utf8_strdown (search_group, -1);

    if (remaining_text != NULL && remaining_text[0] != '\0')
        self->search_lower = g_utf8_strdown (remaining_text, -1);
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

    /* --- Search text filter (uses cached lowered strings) --- */
    if (self->search_group_lower != NULL)
    {
        const gchar *eg_lower = otp_entry_get_group_lower (entry);
        if (g_strstr_len (eg_lower, -1, self->search_group_lower) == NULL)
            return FALSE;
    }

    if (self->search_lower == NULL)
        return TRUE;

    const gchar *account_lower = otp_entry_get_account_lower (entry);
    const gchar *issuer_lower  = otp_entry_get_issuer_lower (entry);

    return (g_strstr_len (account_lower, -1, self->search_lower) != NULL ||
            g_strstr_len (issuer_lower,  -1, self->search_lower) != NULL);
}

/* Show a banner reminding the user to take a backup. We surface it when:
 *   - the user has tokens but has never run an export, OR
 *   - the last successful export is older than BACKUP_AGE_WARN_DAYS.
 * The banner stays hidden when the DB is empty so brand-new users aren't
 * scolded immediately. */
static void
refresh_backup_age_banner (OTPClientWindow *self)
{
    if (self->backup_age_banner == NULL || self->settings == NULL || self->otp_store == NULL)
        return;

    gboolean reveal = FALSE;

    guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->otp_store));
    if (n_items == 0)
        goto apply;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
    DatabaseData *db = app != NULL ? otpclient_application_get_db_data (app) : NULL;
    if (db == NULL) goto apply;
    gint64 last_export = gsettings_common_get_database_time (self->settings, OTPCLIENT_BACKUP_TIMES, db->db_path);
    gint64 snoozed_until = gsettings_common_get_database_time (self->settings, OTPCLIENT_BACKUP_SNOOZES, db->db_path);
    gint64 now = (gint64) g_get_real_time () / G_USEC_PER_SEC;
    const gint64 warn_secs = (gint64) BACKUP_AGE_WARN_DAYS * 24 * 60 * 60;

    if (snoozed_until > now)
        goto apply;

    if (last_export == 0)
    {
        adw_banner_set_title (ADW_BANNER (self->backup_age_banner),
                              _("No backup recorded for this database. Keep an encrypted copy somewhere safe."));
        reveal = TRUE;
    }
    else if (last_export > now)
    {
        /* Clock skew: stored timestamp is in the future (likely NTP correction
         * after a recent export). Treat as fresh and hide the banner. */
    }
    else if (now - last_export > warn_secs)
    {
        gint days = (gint) ((now - last_export) / (60 * 60 * 24));
        g_autofree gchar *msg = g_strdup_printf (
            ngettext ("Your last database backup is %d day old.",
                      "Your last database backup is %d days old.",
                      days),
            days);
        adw_banner_set_title (ADW_BANNER (self->backup_age_banner), msg);
        reveal = TRUE;
    }

apply:
    adw_banner_set_revealed (ADW_BANNER (self->backup_age_banner), reveal);
    gtk_widget_action_set_enabled (GTK_WIDGET (self),
                                   "win.snooze-backup-banner", reveal);
}

static void
update_quarantine_banner (OTPClientWindow *self)
{
    if (self->quarantine_banner == NULL)
        return;
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    DatabaseData *db = app != NULL ? otpclient_application_get_db_data (app) : NULL;
    guint count = db_get_quarantined_count (db);
    gboolean reveal = count > 0 && !otpclient_application_get_app_locked (app);
    if (reveal) {
        g_autofree gchar *title = g_strdup_printf (
            ngettext ("%u token needs repair.", "%u tokens need repair.", count), count);
        adw_banner_set_title (ADW_BANNER (self->quarantine_banner), title);
    }
    adw_banner_set_revealed (ADW_BANNER (self->quarantine_banner), reveal);
    gtk_widget_action_set_enabled (GTK_WIDGET (self), "win.review-quarantine", reveal);
}

static void
update_empty_state (OTPClientWindow *self)
{
    update_quarantine_banner (self);
    if (self->content_stack == NULL || self->otp_store == NULL)
        return;

    /* Pre-list states: locked supersedes everything (don't show the token
     * list, or even the "empty"/"loading" pages, while locked). No DB at all
     * (fresh install) -> "no-db" CTA; an existing DB is being decrypted ->
     * "Unlocking…"; the loaded list is either populated or empty-but-unlocked,
     * handled below. */
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    DatabaseData *db_data = app != NULL ? otpclient_application_get_db_data (app) : NULL;
    const gchar *target_page;
    if (app != NULL && otpclient_application_get_app_locked (app))
        target_page = "locked";
    else if (db_data == NULL)
        target_page = "no-db";
    else if (db_data->in_memory_json_data == NULL)
        target_page = "loading";
    else if (self->filter_model == NULL ||
             g_list_model_get_n_items (gtk_filter_list_model_get_model (self->filter_model)) == 0)
        target_page = "empty";
    else if (self->filter_model != NULL &&
             g_list_model_get_n_items (G_LIST_MODEL (self->filter_model)) == 0)
        target_page = "no-search-results";
    else
        target_page = "list";

    const gchar *current_page = gtk_stack_get_visible_child_name (GTK_STACK (self->content_stack));
    gtk_stack_set_visible_child_name (GTK_STACK (self->content_stack), target_page);

    /* Move focus onto the page's primary widget when it first becomes
     * visible, so the obvious next action (consume a token, add the first
     * token, create the first DB) is one keystroke away rather than buried
     * past the toolbar (issue #445). "loading" is transient - leave focus
     * alone so it doesn't flicker. */
    if (g_strcmp0 (target_page, current_page) != 0)
    {
        GtkWidget *focus_target = NULL;
        if (g_strcmp0 (target_page, "list") == 0)
            focus_target = self->otp_list;
        else if (g_strcmp0 (target_page, "empty") == 0)
            focus_target = self->empty_state_add_button;
        else if (g_strcmp0 (target_page, "no-search-results") == 0)
            focus_target = self->search_empty_clear_button;
        else if (g_strcmp0 (target_page, "no-db") == 0)
            focus_target = self->no_db_create_button;
        else if (g_strcmp0 (target_page, "locked") == 0)
            focus_target = self->locked_unlock_button;

        if (focus_target != NULL)
            gtk_widget_grab_focus (focus_target);
    }

    if (g_strcmp0 (target_page, "list") != 0 && g_strcmp0 (target_page, "empty") != 0)
        return;

    refresh_backup_age_banner (self);
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

void
otpclient_window_show_error_toast (OTPClientWindow *self,
                                    const gchar     *message)
{
    g_return_if_fail (OTPCLIENT_IS_WINDOW (self));
    show_error_toast (self, "%s", message != NULL ? message : "");
}

void
otpclient_window_set_locked_indicator (OTPClientWindow *self,
                                        gboolean         locked)
{
    g_return_if_fail (OTPCLIENT_IS_WINDOW (self));
    if (self->lock_button == NULL)
        return;

    if (locked) {
        gtk_button_set_icon_name (GTK_BUTTON (self->lock_button), "changes-prevent-symbolic");
        gtk_widget_set_tooltip_text (self->lock_button, _("Unlock database"));
    } else {
        gtk_button_set_icon_name (GTK_BUTTON (self->lock_button), "system-lock-screen-symbolic");
        gtk_widget_set_tooltip_text (self->lock_button, _("Lock database"));
    }
}

void
otpclient_window_set_db_actions_enabled (OTPClientWindow *self,
                                          gboolean         enabled)
{
    g_return_if_fail (OTPCLIENT_IS_WINDOW (self));

    /* Every action that reads or mutates token data must be gated by the lock
     * state. win.show-qr in particular renders the secret as a scannable QR. */
    static const gchar * const db_actions[] = {
        "win.add-manual",
        "win.add-qr-file",
        "win.add-qr-webcam",
        "win.add-qr-clipboard",
        "win.import",
        "win.export",
        "win.edit-token",
        "win.delete-token",
        "win.show-qr",
        "win.move-token",
        "win.set-group",
        "win.new-group",
        "win.remove-from-group",
        "win.bulk-set-group",
        "win.bulk-export",
        "win.bulk-delete",
        "win.rename-db",
        "win.set-primary-db",
        "win.remove-db",
        "win.backup-tokens",
        "win.restore-tokens",
    };

    for (gsize i = 0; i < G_N_ELEMENTS (db_actions); i++)
        gtk_widget_action_set_enabled (GTK_WIDGET (self), db_actions[i], enabled);
}

void
otpclient_window_clear_displayed_otps (OTPClientWindow *self)
{
    g_return_if_fail (OTPCLIENT_IS_WINDOW (self));

    GListStore *stores[2] = { self->otp_store, self->cross_db_store };
    for (guint s = 0; s < G_N_ELEMENTS (stores); s++)
    {
        if (stores[s] == NULL)
            continue;
        guint n = g_list_model_get_n_items (G_LIST_MODEL (stores[s]));
        for (guint i = 0; i < n; i++)
        {
            g_autoptr (OTPEntry) entry = g_list_model_get_item (G_LIST_MODEL (stores[s]), i);
            if (entry != NULL)
            {
                otp_entry_set_revealed (entry, FALSE);
                otp_entry_set_otp_value (entry, "");
            }
        }
    }

    g_weak_ref_set (&self->clipboard_owner_entry, NULL);

    if (self->otp_selection != NULL)
        gtk_single_selection_set_selected (self->otp_selection, GTK_INVALID_LIST_POSITION);
}

void
otpclient_window_refresh_content_page (OTPClientWindow *self)
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

    add_bulk_selection_column (self);
    add_text_column (GTK_COLUMN_VIEW (self->otp_list), _("Account"), OTP_COLUMN_ACCOUNT);
    add_text_column (GTK_COLUMN_VIEW (self->otp_list), _("Issuer"), OTP_COLUMN_ISSUER);
    add_text_column (GTK_COLUMN_VIEW (self->otp_list), _("OTP Value"), OTP_COLUMN_VALUE);
    add_validity_column (GTK_COLUMN_VIEW (self->otp_list));
    add_action_column (GTK_COLUMN_VIEW (self->otp_list));

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

    otpclient_window_clear_displayed_otps (OTPCLIENT_WINDOW (widget));
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
action_clear_search (GtkWidget  *widget,
                     const char *action_name,
                     GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;
    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    gtk_editable_set_text (GTK_EDITABLE (self->search_entry), "");
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

    /* Sync search group prefix -> dropdown */
    if (!win->syncing_group_filter && win->group_list_model != NULL)
    {
        const gchar *remaining_text = NULL;
        g_autofree gchar *search_group = parse_search_group (text, &remaining_text);
        (void) remaining_text;

        if (search_group != NULL)
        {
            /* Find matching group in dropdown and select it. The model reads
             * ["All", <groups...>, "Ungrouped"], so both ends are sentinels and
             * only what lies between them is a real group name.
             *
             * i + 1 < n_items rather than i < n_items - 1 because the model is
             * empty, not just sentinel-only, while the database is locked:
             * secure_lock_cleanup splices it to zero and rebuild_group_list
             * refuses to refill it without a database. n_items - 1 then
             * underflowed to 4294967295 and the loop read past the end of the
             * model for as long as the user was willing to wait, two criticals
             * an iteration. Ctrl+F and a bare "group:" on the locked page was
             * the whole recipe. */
            guint n_items = g_list_model_get_n_items (G_LIST_MODEL (win->group_list_model));
            g_autofree gchar *sg_lower = g_utf8_strdown (search_group, -1);
            win->syncing_group_filter = TRUE;
            gboolean found = FALSE;
            for (guint i = 1; i + 1 < n_items; i++)
            {
                const gchar *item = gtk_string_list_get_string (win->group_list_model, i);
                g_autofree gchar *item_lower = g_utf8_strdown (item, -1);
                if (g_strstr_len (item_lower, -1, sg_lower) != NULL)
                {
                    gtk_drop_down_set_selected (GTK_DROP_DOWN (win->group_dropdown), i);
                    g_clear_pointer (&win->active_group_filter, g_free);
                    win->active_group_filter = g_strdup (item);
                    found = TRUE;
                    break;
                }
            }
            if (!found)
            {
                gtk_drop_down_set_selected (GTK_DROP_DOWN (win->group_dropdown), 0);
                g_clear_pointer (&win->active_group_filter, g_free);
            }
            win->syncing_group_filter = FALSE;
        }
        else if (win->active_group_filter != NULL ||
                 gtk_drop_down_get_selected (GTK_DROP_DOWN (win->group_dropdown)) != 0)
        {
            /* A plain text query replaces any previous dropdown group. Keep
             * the dropdown and its filter state in sync with the query. */
            win->syncing_group_filter = TRUE;
            g_clear_pointer (&win->active_group_filter, g_free);
            gtk_drop_down_set_selected (GTK_DROP_DOWN (win->group_dropdown), 0);
            win->syncing_group_filter = FALSE;
        }
    }

    refresh_search_cache (win);
    gtk_filter_changed (GTK_FILTER (win->search_filter), GTK_FILTER_CHANGE_DIFFERENT);
    update_empty_state (win);

    /* Auto-select when search narrows to a single result */
    guint n = g_list_model_get_n_items (G_LIST_MODEL (win->filter_model));
    if (n == 1)
        gtk_single_selection_set_selected (win->otp_selection, 0);
}

static guint find_store_pos_for_entry (OTPClientWindow *self, OTPEntry *entry);

static void
search_entry_activate (GtkEntry        *entry,
                       OTPClientWindow *self)
{
    (void) entry;

    /* Enter activates the selected token and closes search. */
    trigger_otp_action_for_entry (self, OTP_ENTRY (gtk_single_selection_get_selected_item (self->otp_selection)));

    gtk_search_bar_set_search_mode (GTK_SEARCH_BAR (self->search_bar), FALSE);
}

static void
database_row_selected (GtkListBox      *box,
                       GtkListBoxRow   *row,
                       OTPClientWindow *self)
{
    (void) box;

    if (row == NULL)
        return;

    gint index = gtk_list_box_row_get_index (row);
    if (index < 0)
        return;

    g_autoptr (DatabaseEntry) entry = g_list_model_get_item (
        G_LIST_MODEL (self->db_store), (guint) index);
    if (entry == NULL)
        return;

    if (database_entry_get_missing (entry))
        return;

    const gchar *path = database_entry_get_path (entry);
    if (path == NULL || path[0] == '\0')
        return;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
        return;

    /* Suppress redundant switches:
     *  - Same DB already loaded -> nothing to do.
     *  - Cold start: init_database() calls select_database() before it
     *    sets db_data, so this fires once with current == NULL. If the
     *    row matches the path init_database is about to load, let that
     *    flow run; otherwise we'd stack a second password dialog. */
    DatabaseData *current = otpclient_application_get_db_data (app);
    if (current != NULL && g_strcmp0 (current->db_path, path) == 0)
        return;
    if (current == NULL)
    {
        g_autofree gchar *cfg_path = gui_misc_get_db_path_from_cfg ();
        if (g_strcmp0 (cfg_path, path) == 0)
            return;
    }

    /* Refuse the switch while an unlock worker is still reading db_data;
     * otherwise otpclient_application_switch_to_db -> set_db_data(NULL)
     * would free db_data under the worker's feet.
     *
     * Refuse it while the window is locked too. The sidebar stays reachable on
     * the locked page (#467 leaves the toolbar live after Escape), and a switch
     * from there swaps the active database behind the lock screen: the user
     * comes back, unlocks what they think is the database they left, and gets a
     * different one. Locking is about the database that was open.
     *
     * Either way, revert the visual row selection to the loaded database so the
     * sidebar does not mislead the user about which one is active. */
    if (otpclient_application_is_unlocking (app) || window_is_locked (self))
    {
        otpclient_window_show_error_toast (self,
            otpclient_application_is_unlocking (app)
                ? _("Please wait, the database is still being unlocked.")
                : _("Unlock the current database before switching to another one."));
        if (current != NULL && current->db_path != NULL)
        {
            guint n = g_list_model_get_n_items (G_LIST_MODEL (self->db_store));
            for (guint i = 0; i < n; i++)
            {
                g_autoptr (DatabaseEntry) e = g_list_model_get_item (
                    G_LIST_MODEL (self->db_store), i);
                if (g_strcmp0 (database_entry_get_path (e), current->db_path) == 0)
                {
                    otpclient_window_select_database (self, (gint) i);
                    break;
                }
            }
        }
        return;
    }

    otpclient_application_switch_to_db (app, path);
    otpclient_window_sync_active_flag (self);
}

static void on_db_entry_name_changed (DatabaseEntry *entry,
                                      GParamSpec    *pspec,
                                      AdwActionRow  *row);

static void on_db_entry_path_changed (DatabaseEntry *entry,
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

static void
on_db_entry_missing_icon_changed (DatabaseEntry *entry,
                                  GParamSpec    *pspec,
                                  GtkWidget     *icon)
{
    (void) pspec;
    gtk_widget_set_visible (icon, database_entry_get_missing (entry));
}

static void
on_db_entry_missing_row_changed (DatabaseEntry *entry,
                                 GParamSpec    *pspec,
                                 GtkWidget     *row)
{
    (void) pspec;
    if (database_entry_get_missing (entry))
        gtk_widget_add_css_class (row, "dim-label");
    else
        gtk_widget_remove_css_class (row, "dim-label");
}

static void
on_db_entry_active_changed (DatabaseEntry *entry,
                            GParamSpec    *pspec,
                            GtkWidget     *row)
{
    (void) pspec;
    if (database_entry_get_active (entry))
        gtk_widget_add_css_class (row, "active-database");
    else
        gtk_widget_remove_css_class (row, "active-database");
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

    GtkWidget *check_icon = gtk_image_new_from_icon_name ("starred-symbolic");
    gtk_widget_set_valign (check_icon, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (check_icon, _("Default database (loaded on startup)"));
    gtk_widget_set_visible (check_icon, database_entry_get_primary (entry));
    adw_action_row_add_suffix (row, check_icon);

    GtkWidget *warning_icon = gtk_image_new_from_icon_name ("dialog-warning-symbolic");
    gtk_widget_set_valign (warning_icon, GTK_ALIGN_CENTER);
    gtk_widget_set_tooltip_text (warning_icon, _("Database file not found"));
    gtk_widget_set_visible (warning_icon, database_entry_get_missing (entry));
    adw_action_row_add_suffix (row, warning_icon);

    if (database_entry_get_missing (entry))
        gtk_widget_add_css_class (GTK_WIDGET (row), "dim-label");

    if (database_entry_get_active (entry))
        gtk_widget_add_css_class (GTK_WIDGET (row), "active-database");

    g_signal_connect_object (entry, "notify::primary",
                             G_CALLBACK (on_db_entry_primary_changed), check_icon, 0);

    g_signal_connect_object (entry, "notify::missing",
                             G_CALLBACK (on_db_entry_missing_icon_changed), warning_icon, 0);

    g_signal_connect_object (entry, "notify::missing",
                             G_CALLBACK (on_db_entry_missing_row_changed), row, 0);

    g_signal_connect_object (entry, "notify::active",
                             G_CALLBACK (on_db_entry_active_changed), row, 0);

    g_signal_connect_object (entry, "notify::name",
                             G_CALLBACK (on_db_entry_name_changed), row, 0);

    g_signal_connect_object (entry, "notify::path",
                             G_CALLBACK (on_db_entry_path_changed), row, 0);

    return GTK_WIDGET (row);
}

static void
ensure_sidebar_css_provider (void)
{
    static gsize loaded = 0;
    if (g_once_init_enter (&loaded))
    {
        GtkCssProvider *provider = gtk_css_provider_new ();
        gtk_css_provider_load_from_string (provider,
            "row.active-database .title { font-weight: bold; }");
        gtk_style_context_add_provider_for_display (
            gdk_display_get_default (),
            GTK_STYLE_PROVIDER (provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref (provider);
        g_once_init_leave (&loaded, 1);
    }
}

static void
setup_database_list (OTPClientWindow *self)
{
    ensure_sidebar_css_provider ();

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
otpclient_window_sync_active_flag (OTPClientWindow *self)
{
    g_return_if_fail (OTPCLIENT_IS_WINDOW (self));

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    DatabaseData *db_data = (app != NULL)
        ? otpclient_application_get_db_data (app) : NULL;
    const gchar *active_path = (db_data != NULL) ? db_data->db_path : NULL;

    guint n = g_list_model_get_n_items (G_LIST_MODEL (self->db_store));
    for (guint i = 0; i < n; i++)
    {
        g_autoptr (DatabaseEntry) entry = g_list_model_get_item (
            G_LIST_MODEL (self->db_store), i);
        gboolean is_active = active_path != NULL
            && g_strcmp0 (database_entry_get_path (entry), active_path) == 0;
        database_entry_set_active (entry, is_active);
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
    otpclient_window_invalidate_cross_db (self);

    /* If no primary is set yet, this is the first DB the user has - make
     * it the default. Don't gate on store size: at startup the sidebar is
     * repopulated entry by entry, and the first one would otherwise
     * clobber whatever the user explicitly chose as primary. */
    g_autofree gchar *current_primary = gui_misc_get_db_path_from_cfg ();
    if (current_primary == NULL || current_primary[0] == '\0')
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

static void
clipboard_forget (OTPClientWindow *self)
{
    if (self->clipboard_clear_timer_id != 0) {
        g_source_remove (self->clipboard_clear_timer_id);
        self->clipboard_clear_timer_id = 0;
    }
    g_clear_object (&self->clipboard_content);
    g_weak_ref_set (&self->clipboard_owner_entry, NULL);
}

static void
on_clipboard_changed (GdkClipboard *clipboard, OTPClientWindow *self)
{
    if (self->clipboard_content != NULL &&
        gdk_clipboard_get_content (clipboard) != self->clipboard_content)
        clipboard_forget (self);
}

static gboolean
clipboard_clear_cb (gpointer user_data)
{
    OTPClientWindow *self = user_data;
    self->clipboard_clear_timer_id = 0;
    otpclient_window_clear_clipboard_now (self);
    return G_SOURCE_REMOVE;
}

void
otpclient_window_clear_clipboard_now (OTPClientWindow *self)
{
    g_return_if_fail (OTPCLIENT_IS_WINDOW (self));
    GdkDisplay *display = gdk_display_get_default ();
    GdkClipboard *clipboard = display != NULL ? gdk_display_get_clipboard (display) : NULL;
    if (clipboard != NULL && self->clipboard_content != NULL &&
        gdk_clipboard_get_content (clipboard) == self->clipboard_content)
        gdk_clipboard_set_content (clipboard, NULL);
    clipboard_forget (self);
}

/* Push the entry's current OTP to the clipboard, mark the entry as the
 * clipboard owner, re-arm the auto-clear timer, and fire the copy
 * notification. Shared by the user-click path and the TOTP auto-roll path
 * in otp_refresh_tick. */
static void
copy_otp_to_clipboard_and_notify (OTPClientWindow *self, OTPEntry *entry)
{
    const gchar *otp_value = otp_entry_get_otp_value (entry);
    if (otp_value == NULL || otp_value[0] == '\0')
        return;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));

    GdkDisplay *display = gdk_display_get_default ();
    if (display == NULL)
        return;
    GdkClipboard *clipboard = gdk_display_get_clipboard (display);
    if (clipboard == NULL)
        return;
    g_clear_object (&self->clipboard_content);
    self->clipboard_content = gdk_content_provider_new_typed (G_TYPE_STRING, otp_value);
    if (!gdk_clipboard_set_content (clipboard, self->clipboard_content)) {
        clipboard_forget (self);
        return;
    }

    g_weak_ref_set (&self->clipboard_owner_entry, entry);

    if (self->clipboard_clear_timer_id != 0)
    {
        g_source_remove (self->clipboard_clear_timer_id);
        self->clipboard_clear_timer_id = 0;
    }
    guint clear_timeout = 30;
    if (app != NULL)
        clear_timeout = otpclient_application_get_clipboard_clear_timeout (app);
    if (clear_timeout > 0)
        self->clipboard_clear_timer_id = g_timeout_add_seconds (clear_timeout,
                                                                  clipboard_clear_cb, self);

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

/* Explicit activation persists HOTP counters before revealing or copying,
 * then schedules clipboard cleanup and notification. */
static void
trigger_otp_action_for_entry (OTPClientWindow *self, OTPEntry *entry)
{
    if (entry == NULL || self->bulk_mode)
        return;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));

    if (window_is_locked (self) || app == NULL)
        return;
    /* g_ascii_strcasecmp() g_return_val_if_fail()s to 0, which reads as "equal",
     * so a missing type would be mistaken for HOTP and would open a database
     * transaction on a TOTP row. Guard rather than trust the setter. */
    const gchar *type = otp_entry_get_otp_type (entry);
    gboolean is_hotp = type != NULL && g_ascii_strcasecmp (type, "HOTP") == 0;
    if (is_hotp && otp_entry_get_db_path (entry) != NULL) {
        g_autofree gchar *path = g_strdup (otp_entry_get_db_path (entry));
        if (!otpclient_application_is_unlocking (app))
            otpclient_application_switch_to_db (app, path);
        return;
    }
    if (is_hotp) {
        DatabaseData *db = otpclient_application_get_db_data (app);
        guint pos = find_store_pos_for_entry (self, entry);
        if (db == NULL || pos == GTK_INVALID_LIST_POSITION)
            return;
        gsize index = pos;
        GError *err = NULL;
        g_autoptr (GPtrArray) results = db_generate_hotp (db, &index, 1, &err);
        if (results == NULL) {
            show_error_toast (self, "%s", err != NULL ? err->message : _("Failed to save HOTP counter"));
            g_clear_error (&err);
            return;
        }
        DbHotpResult *result = g_ptr_array_index (results, 0);
        otp_entry_set_counter (entry, result->next_counter);
        otp_entry_set_otp_value (entry, result->code);
    } else {
        otp_entry_update_otp (entry);
    }

    const gchar *otp_value = otp_entry_get_otp_value (entry);
    if (otp_value == NULL || otp_value[0] == '\0')
        return;

    copy_otp_to_clipboard_and_notify (self, entry);

    /* Reveal-on-hide: tie the reveal lifetime to the validity window. For
     * TOTP, just flip revealed=TRUE and let otp_refresh_tick decide what
     * happens at rotation. For HOTP (no rotation), use a fixed short timer.
     * No-op when hide-by-default is off. */
    if (app != NULL && otpclient_application_get_hide_otps (app))
    {
        if (is_hotp)
        {
            otp_entry_reveal_for (entry, HOTP_REVEAL_SECONDS);
        }
        else
        {
            otp_entry_set_revealed (entry, TRUE);
            /* Re-click on an already-revealed row: set_revealed() no-ops,
             * so the FALSE->TRUE auto-reset of roll_consumed never fires.
             * Reset explicitly so the user gets a fresh roll cycle. */
            otp_entry_reset_roll (entry);
        }
    }
}

static void
action_activate_token (GtkWidget *widget, const gchar *name, GVariant *parameter)
{
    (void) name;
    (void) parameter;
    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    trigger_otp_action_for_entry (self, OTP_ENTRY (gtk_single_selection_get_selected_item (self->otp_selection)));
}

static void
on_token_action_clicked (GtkButton *button, GtkListItem *item)
{
    GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (button));
    if (OTPCLIENT_IS_WINDOW (root))
        trigger_otp_action_for_entry (OTPCLIENT_WINDOW (root), OTP_ENTRY (gtk_list_item_get_item (item)));
}

/* The Action-column button and the context-menu item must agree on what the
 * primary action is for a row, so both ask here. The type test matches the one
 * in trigger_otp_action_for_entry, NULL check included: on a NULL type,
 * g_ascii_strcasecmp() trips its g_return_val_if_fail() and returns 0, which
 * reads as "equal", so a missing type would otherwise pass for HOTP. */
static void
token_action_for_entry (OTPEntry     *entry,
                        const gchar **label_out,
                        const gchar **icon_out)
{
    const gchar *type = entry != NULL ? otp_entry_get_otp_type (entry) : NULL;
    gboolean hotp = type != NULL && g_ascii_strcasecmp (type, "HOTP") == 0;

    const gchar *label, *icon;

    if (hotp && otp_entry_get_db_path (entry) != NULL) {
        label = _("Open database");
        icon = "document-open-symbolic";
    } else if (hotp) {
        label = _("Generate");
        icon = "view-refresh-symbolic";
    } else {
        label = _("Copy");
        icon = "edit-copy-symbolic";
    }

    if (label_out != NULL)
        *label_out = label;
    if (icon_out != NULL)
        *icon_out = icon;
}

static void
action_column_setup (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
    (void) factory;
    (void) data;
    GtkWidget *button = gtk_button_new ();
    gtk_widget_set_valign (button, GTK_ALIGN_CENTER);
    gtk_widget_add_css_class (button, "flat");
    g_signal_connect_object (button, "clicked", G_CALLBACK (on_token_action_clicked), item, 0);
    gtk_list_item_set_child (item, button);
}

static void
action_column_bind (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
    (void) factory;
    (void) data;
    OTPEntry *entry = OTP_ENTRY (gtk_list_item_get_item (item));
    const gchar *label = NULL, *icon = NULL;
    token_action_for_entry (entry, &label, &icon);
    GtkWidget *button = gtk_list_item_get_child (item);
    gtk_button_set_icon_name (GTK_BUTTON (button), icon);

    /* pick_entry_at() resolves a click by walking the widget chain for this
     * data. Only the text-column labels carry it, so without this a right-click
     * landing on the Action cell fails to select the row. */
    g_object_set_data (G_OBJECT (button), "otp-entry", entry);

    /* The button shows an icon only, so the word lives in the tooltip and in the
     * accessible name. */
    gtk_accessible_update_property (GTK_ACCESSIBLE (button),
                                    GTK_ACCESSIBLE_PROPERTY_LABEL, label, -1);

    /* Fall back to the issuer when the account is blank, otherwise the tooltip
     * reads "Copy: " and adds nothing to the button label. */
    const gchar *subject = otp_entry_get_account (entry);
    if (subject == NULL || *subject == '\0')
        subject = otp_entry_get_issuer (entry);
    if (subject != NULL && *subject != '\0') {
        g_autofree gchar *tip = g_strdup_printf ("%s: %s", label, subject);
        gtk_widget_set_tooltip_text (button, tip);
    } else {
        gtk_widget_set_tooltip_text (button, label);
    }
}

/* Same reasoning as otp_text_column_unbind: the button is recycled across rows
 * and holds a raw pointer to the entry it was last bound to. */
static void
action_column_unbind (GtkSignalListItemFactory *factory, GtkListItem *item, gpointer data)
{
    (void) factory;
    (void) data;
    GtkWidget *button = gtk_list_item_get_child (item);
    if (button != NULL)
        g_object_set_data (G_OBJECT (button), "otp-entry", NULL);
}

static void
add_action_column (GtkColumnView *view)
{
    GtkListItemFactory *factory = gtk_signal_list_item_factory_new ();
    g_signal_connect (factory, "setup", G_CALLBACK (action_column_setup), NULL);
    g_signal_connect (factory, "bind", G_CALLBACK (action_column_bind), NULL);
    g_signal_connect (factory, "unbind", G_CALLBACK (action_column_unbind), NULL);
    GtkColumnViewColumn *column = gtk_column_view_column_new (_("Action"), factory);
    /* Icon buttons are all one size, so pin the width instead of letting the
     * column stretch to whatever the widest row needs. */
    gtk_column_view_column_set_fixed_width (column, 64);
    gtk_column_view_column_set_resizable (column, FALSE);
    gtk_column_view_append_column (view, column);
    g_object_unref (column);
}

static void on_popover_closed (GtkPopover *popover, gpointer user_data);
static void popup_menu_at (GtkWidget *popover, GtkWidget *over, double x, double y);

static gboolean
popup_token_menu_from_keyboard (OTPClientWindow *self)
{
    if (window_is_locked (self) ||
        gtk_single_selection_get_selected (self->otp_selection) == GTK_INVALID_LIST_POSITION)
        return FALSE;

    GtkBuilder *builder = gtk_builder_new_from_resource (
        "/com/github/paolostivanin/OTPClient/ui/context-menus.ui");
    GMenuModel *menu = G_MENU_MODEL (gtk_builder_get_object (builder, "token_context_menu"));
    GtkWidget *popover = gtk_popover_menu_new_from_model (menu);
    double x = gtk_widget_get_width (self->otp_list) / 2.0;
    double y = gtk_widget_get_height (self->otp_list) / 2.0;
    popup_menu_at (popover, self->otp_list, x, y);
    g_object_unref (builder);
    return TRUE;
}

static gboolean
on_token_key_pressed (GtkEventControllerKey *controller, guint keyval,
                      guint keycode, GdkModifierType state, gpointer data)
{
    (void) controller;
    (void) keycode;
    OTPClientWindow *self = data;
    gboolean context_menu = keyval == GDK_KEY_Menu ||
        (keyval == GDK_KEY_F10 && (state & GDK_SHIFT_MASK) != 0);
    if (context_menu)
        return popup_token_menu_from_keyboard (self);

    gboolean activate = ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter) &&
                         (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) == 0) ||
                        ((keyval == GDK_KEY_c || keyval == GDK_KEY_C) &&
                         (state & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK)) == GDK_CONTROL_MASK);
    if (!activate)
        return FALSE;
    trigger_otp_action_for_entry (self, OTP_ENTRY (gtk_single_selection_get_selected_item (self->otp_selection)));
    return TRUE;
}

/* Double-click: the row turns the second press into list.activate-item and the
 * column view re-emits it here. Enter is bound to the same action, but
 * on_token_key_pressed sees it first in the capture phase and returns TRUE, so
 * the keybinding never runs and one press stays one activation. Resolve the
 * entry from the position handed to us rather than from the selection: the two
 * agree, since activation selects the row first, but the position is the one
 * that says which row was clicked. */
static void
on_token_row_activated (GtkColumnView *view, guint position, gpointer data)
{
    (void) view;
    OTPClientWindow *self = data;
    g_autoptr (OTPEntry) entry = OTP_ENTRY (
        g_list_model_get_item (G_LIST_MODEL (self->otp_selection), position));
    trigger_otp_action_for_entry (self, entry);
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

static void on_db_modified (gpointer user_data);

/* Resolve the current selection to a JSON-array index.
 *
 * The selection wraps sort+filter+store, so the bare selection position
 * reflects the filtered+sorted view - not the underlying store or the
 * JSON. on_db_modified() rebuilds otp_store from the JSON array in
 * order, so the store position equals the JSON index. Look up the
 * selected item itself rather than trusting the selection position.
 *
 * Returns GTK_INVALID_LIST_POSITION when nothing is selected, the
 * selection is a cross-DB (read-only) result, or the entry no longer
 * exists in the store.
 */
static guint
selected_writable_json_index (OTPClientWindow *self)
{
    OTPEntry *entry = OTP_ENTRY (gtk_single_selection_get_selected_item (self->otp_selection));
    if (entry == NULL)
        return GTK_INVALID_LIST_POSITION;
    if (otp_entry_get_db_name (entry) != NULL)
        return GTK_INVALID_LIST_POSITION;
    return find_store_pos_for_entry (self, entry);
}

guint
otpclient_window_selected_writable_json_index (OTPClientWindow *self)
{
    return selected_writable_json_index (self);
}

/*
 * Find the OTPEntry and row widget at coordinates (x, y) relative to
 * the GtkColumnView.  Returns the OTPEntry or NULL.
 * If row_out is non-NULL, the row-level widget is written there.
 *
 * The GtkColumnView hierarchy is:
 *   GtkColumnView -> GtkColumnListView -> GtkColumnViewRowWidget
 *     -> GtkColumnViewCell -> our child widget (GtkLabel/GtkBox)
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

    if (window_is_locked (self))
        return NULL;

    /* Disable DnD while search filter is active */
    const gchar *search_text = gtk_editable_get_text (GTK_EDITABLE (self->search_entry));
    if (search_text != NULL && search_text[0] != '\0')
        return NULL;

    /* The selection model sits on top of the sort model, so the selected
     * index is a sorted-view position. Resolve the entry and carry its
     * position in the underlying (unsorted) store, which is what on_drop
     * needs regardless of the active sort or group filter. */
    GObject *item = gtk_single_selection_get_selected_item (self->otp_selection);
    if (item == NULL)
        return NULL;

    guint store_pos = find_store_pos_for_entry (self, OTP_ENTRY (item));
    if (store_pos == GTK_INVALID_LIST_POSITION)
        return NULL;

    GValue value = G_VALUE_INIT;
    g_value_init (&value, G_TYPE_UINT);
    g_value_set_uint (&value, store_pos);

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

    /* on_drag_prepare stored the source's position in the unsorted store. */
    guint source_pos = g_value_get_uint (value);

    g_autoptr (OTPEntry) source_entry = g_list_model_get_item (
        G_LIST_MODEL (self->otp_store), source_pos);
    if (source_entry == NULL)
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
                /* update_db's failure path has already put the JSON back the
                 * way it was, so the store is now the only thing holding the
                 * new order and "store index == JSON index" no longer holds.
                 * Everything that addresses a token by position reads the wrong
                 * one after that: Generate advances and persists a different
                 * account's HOTP counter and shows that account's code, and
                 * Edit and Delete land on the wrong row too. Rebuild from the
                 * JSON so the visible order is the saved one. */
                on_db_modified (self);
            }
        }
    }

    return TRUE;
}

static void
setup_dnd (OTPClientWindow *self)
{
    self->dnd_css_provider = gtk_css_provider_new ();
    /* 3 px solid bar plus a soft outer glow so the indicator stays visible across
     * light and dark themes regardless of the row's own background. The negative
     * margin pulls the bar to the row edge so adjacent rows' borders don't cancel
     * out the highlight visually. */
    gtk_css_provider_load_from_string (self->dnd_css_provider,
        "row.drop-above {"
        "  box-shadow: inset 0 3px 0 0 @accent_bg_color, 0 -1px 4px alpha(@accent_bg_color, 0.4);"
        "}"
        "row.drop-below {"
        "  box-shadow: inset 0 -3px 0 0 @accent_bg_color, 0 1px 4px alpha(@accent_bg_color, 0.4);"
        "}");
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

/* The key is otherwise read once, at construction, and written once, at close,
 * so a settings import lands in GSettings and the open window never hears about
 * it. Only the split view is moved here: the toggle button already follows it
 * through notify::show-sidebar. */
static void
on_show_sidebar_key_changed (GSettings       *settings,
                             const gchar     *key,
                             OTPClientWindow *self)
{
    (void) key;

    gboolean show = g_settings_get_boolean (settings, "show-sidebar");
    if (show != adw_overlay_split_view_get_show_sidebar (ADW_OVERLAY_SPLIT_VIEW (self->split_view)))
        adw_overlay_split_view_set_show_sidebar (ADW_OVERLAY_SPLIT_VIEW (self->split_view), show);
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
    if (win->disposing) {
        G_OBJECT_CLASS (otpclient_window_parent_class)->dispose (object);
        return;
    }
    win->disposing = TRUE;
    otpclient_window_undo_clear (win);

    if (win->cross_db_cancellable != NULL)
        g_cancellable_cancel (win->cross_db_cancellable);
    if (win->webcam_cancellable != NULL)
        g_cancellable_cancel (win->webcam_cancellable);
    close_webcam_progress (win);
    if (win->clipboard_cancellable != NULL)
        g_cancellable_cancel (win->clipboard_cancellable);
    if (win->file_dialog_cancellable != NULL)
        g_cancellable_cancel (win->file_dialog_cancellable);

    /* M2: actively clear the clipboard on dispose, not just stop the timer.
     * Without this, an OTP copied just before window close (or app lock) sits
     * in the system clipboard until the user's next copy. */
    otpclient_window_clear_clipboard_now (win);

    GdkDisplay *clipboard_display = gdk_display_get_default ();
    if (clipboard_display != NULL)
        g_signal_handlers_disconnect_by_data (gdk_display_get_clipboard (clipboard_display), win);

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
    g_clear_pointer (&win->cross_db_monitors, g_hash_table_unref);
    g_clear_object (&win->flatten_model);
    g_clear_object (&win->cross_db_cancellable);
    g_clear_object (&win->webcam_cancellable);
    g_clear_object (&win->clipboard_cancellable);
    g_clear_object (&win->file_dialog_cancellable);
    g_clear_object (&win->group_list_model);
    g_clear_object (&win->bulk_group_model);
    g_clear_pointer (&win->bulk_selected_entries, g_hash_table_unref);
    g_clear_pointer (&win->active_group_filter, g_free);
    g_clear_pointer (&win->search_lower, g_free);
    g_clear_pointer (&win->search_group_lower, g_free);
    g_clear_object (&win->settings);

    if (win->dnd_css_provider != NULL)
    {
        /* L4: gtk_widget_get_display can return NULL during dispose if the
         * widget is already detached (Wayland session teardown). Skip the
         * remove rather than crashing in gtk_style_context_remove_provider_for_display. */
        GdkDisplay *display = gtk_widget_get_display (GTK_WIDGET (win));
        if (display != NULL)
            gtk_style_context_remove_provider_for_display (
                display,
                GTK_STYLE_PROVIDER (win->dnd_css_provider));
        g_clear_object (&win->dnd_css_provider);
    }

    gtk_widget_dispose_template (GTK_WIDGET (object), OTPCLIENT_TYPE_WINDOW);
    G_OBJECT_CLASS (otpclient_window_parent_class)->dispose (object);
}

static void
otpclient_window_finalize (GObject *object)
{
    OTPClientWindow *self = OTPCLIENT_WINDOW (object);
    g_weak_ref_clear (&self->clipboard_owner_entry);
    G_OBJECT_CLASS (otpclient_window_parent_class)->finalize (object);
}

void
otpclient_window_secure_lock_cleanup (OTPClientWindow *self)
{
    g_return_if_fail (OTPCLIENT_IS_WINDOW (self));
    otpclient_window_undo_clear (self);
    if (self->bulk_mode_button != NULL)
        gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (self->bulk_mode_button), FALSE);
    if (self->bulk_selected_entries != NULL)
        g_hash_table_remove_all (self->bulk_selected_entries);

    /* Snapshot the stack: closing dialogs changes the live model. Clear now,
     * even if an asynchronous file chooser still holds a dialog reference. */
    g_autoptr (GListModel) dialogs = adw_application_window_get_dialogs (ADW_APPLICATION_WINDOW (self));
    g_autoptr (GPtrArray) closing = g_ptr_array_new_with_free_func (g_object_unref);
    for (guint i = 0; i < g_list_model_get_n_items (dialogs); i++)
        g_ptr_array_add (closing, g_list_model_get_item (dialogs, i));
    for (guint i = closing->len; i > 0; i--) {
        AdwDialog *dialog = g_ptr_array_index (closing, i - 1);
        sensitive_dialog_clear (dialog);
        adw_dialog_force_close (dialog);
    }

    otpclient_window_stop_otp_timer (self);
    otpclient_window_clear_clipboard_now (self);
    otpclient_window_clear_displayed_otps (self);

    if (self->cross_db_cancellable != NULL)
        g_cancellable_cancel (self->cross_db_cancellable);
    if (self->webcam_cancellable != NULL)
        g_cancellable_cancel (self->webcam_cancellable);
    close_webcam_progress (self);
    if (self->clipboard_cancellable != NULL)
        g_cancellable_cancel (self->clipboard_cancellable);
    if (self->file_dialog_cancellable != NULL)
        g_cancellable_cancel (self->file_dialog_cancellable);
    g_clear_object (&self->cross_db_cancellable);
    g_clear_object (&self->webcam_cancellable);
    g_clear_object (&self->clipboard_cancellable);
    g_clear_object (&self->file_dialog_cancellable);

    self->cross_db_loading = FALSE;
    self->cross_db_loaded = FALSE;
    if (self->cross_db_monitors != NULL)
        g_hash_table_remove_all (self->cross_db_monitors);
    cross_db_deactivate (self);
    g_clear_object (&self->flatten_model);
    if (self->cross_db_store != NULL)
        g_list_store_remove_all (self->cross_db_store);
    if (self->otp_store != NULL)
        g_list_store_remove_all (self->otp_store);

    g_clear_pointer (&self->search_lower, g_free);
    g_clear_pointer (&self->search_group_lower, g_free);
    g_clear_pointer (&self->active_group_filter, g_free);
    if (self->group_list_model != NULL) {
        guint groups = g_list_model_get_n_items (
            G_LIST_MODEL (self->group_list_model));
        /* Splicing to zero makes GtkDropDown emit notify::selected with
         * GTK_INVALID_LIST_POSITION. The flag suppresses the handler for the
         * synchronous emission; the handler itself also treats an invalid
         * position as "no selection", covering a deferred one. */
        self->syncing_group_filter = TRUE;
        gtk_string_list_splice (self->group_list_model, 0, groups, NULL);
        self->syncing_group_filter = FALSE;
    }
    if (self->search_entry != NULL)
        gtk_editable_set_text (GTK_EDITABLE (self->search_entry), "");
}

GListStore *
otpclient_window_get_otp_store (OTPClientWindow *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_WINDOW (self), NULL);
    return self->otp_store;
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

    g_clear_object (&self->bulk_group_model);
    self->bulk_group_model = gtk_string_list_new (NULL);
    gtk_string_list_append (self->bulk_group_model, _("Ungrouped"));
    for (GList *l = group_names; l != NULL; l = l->next)
        gtk_string_list_append (self->bulk_group_model, (const gchar *) l->data);
    gtk_drop_down_set_model (GTK_DROP_DOWN (self->bulk_group_dropdown),
                             G_LIST_MODEL (self->bulk_group_model));
    gtk_drop_down_set_selected (GTK_DROP_DOWN (self->bulk_group_dropdown),
                               GTK_INVALID_LIST_POSITION);

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
    guint n_items = g_list_model_get_n_items (G_LIST_MODEL (self->group_list_model));
    guint restore_pos = 0; /* default to "All" */
    if (prev_filter != NULL)
    {
        for (guint i = 0; i < n_items; i++)
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

    /* syncing_group_filter suppresses on_group_dropdown_changed, so the
     * effective filter must be updated here. If the previously selected group
     * disappeared (last token removed/moved), restore_pos falls back to "All"
     * or "Ungrouped"; leaving active_group_filter pointing at the dead group
     * would keep filtering it out and show an empty list labelled "All". */
    g_clear_pointer (&self->active_group_filter, g_free);
    if (restore_pos == 0)
        self->active_group_filter = NULL;
    else if (restore_pos == n_items - 1)
        self->active_group_filter = g_strdup ("");
    else
        self->active_group_filter = g_strdup (
            gtk_string_list_get_string (self->group_list_model, restore_pos));
    self->syncing_group_filter = FALSE;

    if (g_strcmp0 (prev_filter, self->active_group_filter) != 0)
        gtk_filter_changed (GTK_FILTER (self->search_filter), GTK_FILTER_CHANGE_DIFFERENT);

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
    guint n = (self->group_list_model != NULL)
        ? g_list_model_get_n_items (G_LIST_MODEL (self->group_list_model)) : 0;

    g_clear_pointer (&self->active_group_filter, g_free);

    if (selected == GTK_INVALID_LIST_POSITION || selected >= n)
    {
        /* No real selection. secure_lock_cleanup splices the group model to
         * zero while the app is locked, and GtkDropDown answers with
         * GTK_INVALID_LIST_POSITION (0xFFFFFFFF): on guint, the old
         * `selected == n - 1` comparison underflowed to TRUE and recorded the
         * empty-string "Ungrouped" sentinel as the user's choice, which
         * rebuild_group_list then faithfully restored - filtering every
         * grouped token out after each lock/unlock cycle. Treat "no
         * selection" as "All"; never as a filter the user chose. */
        self->active_group_filter = NULL;
    }
    else if (selected == 0)
    {
        /* "All" - no group filter */
        self->active_group_filter = NULL;
    }
    else if (selected == n - 1)
    {
        /* "Ungrouped" - empty string sentinel */
        self->active_group_filter = g_strdup ("");
    }
    else
    {
        const gchar *group_name = gtk_string_list_get_string (self->group_list_model, selected);
        self->active_group_filter = g_strdup (group_name);
    }

    refresh_search_cache (self);
    gtk_filter_changed (GTK_FILTER (self->search_filter), GTK_FILTER_CHANGE_DIFFERENT);
    update_empty_state (self);
}

static void
on_db_modified (gpointer user_data)
{
    OTPClientWindow *self = OTPCLIENT_WINDOW (user_data);
    if (self->bulk_selected_entries != NULL)
        g_hash_table_remove_all (self->bulk_selected_entries);
    update_bulk_selection_count (self);
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
        return;

    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL || db_data->in_memory_json_data == NULL)
        return;

    GListStore *store = self->otp_store;



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
    update_quarantine_banner (self);

}

void
otpclient_window_database_modified (OTPClientWindow *self)
{
    on_db_modified (self);
}

typedef struct {
    GWeakRef window_ref;
    DatabaseData *db_data;
    GtkWidget *list_box;
    AdwAlertDialog *dialog;
} QuarantineReview;

typedef struct {
    GWeakRef review_ref;
    DatabaseData *db_data;
    guint lock_generation;
    guint index;
    json_t *original;
    GtkWidget *fields[9];
} QuarantineRepair;

enum {
    REPAIR_LABEL,
    REPAIR_ISSUER,
    REPAIR_GROUP,
    REPAIR_TYPE,
    REPAIR_SECRET,
    REPAIR_ALGORITHM,
    REPAIR_DIGITS,
    REPAIR_PERIOD,
    REPAIR_COUNTER
};

static void
quarantine_review_free (gpointer data)
{
    QuarantineReview *review = data;
    g_weak_ref_clear (&review->window_ref);
    database_data_unref (review->db_data);
    g_free (review);
}

static void
quarantine_repair_free (QuarantineRepair *repair)
{
    if (repair == NULL)
        return;
    g_weak_ref_clear (&repair->review_ref);
    database_data_unref (repair->db_data);
    g_clear_pointer (&repair->original, json_decref);
    g_free (repair);
}

static gchar *
quarantine_field_text (json_t      *token,
                       const gchar *key,
                       const gchar *fallback)
{
    json_t *value = json_object_get (token, key);
    if (json_is_string (value))
        return g_strdup (json_string_value (value));
    if (json_is_integer (value))
        return g_strdup_printf ("%" JSON_INTEGER_FORMAT, json_integer_value (value));
    return g_strdup (fallback ? fallback : "");
}

static GtkWidget *
quarantine_repair_add_field (GtkGrid      *grid,
                             guint         row,
                             const gchar  *title,
                             const gchar  *value,
                             gboolean      password)
{
    GtkWidget *label = gtk_label_new (title);
    gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
    gtk_label_set_wrap (GTK_LABEL (label), TRUE);
    gtk_label_set_max_width_chars (GTK_LABEL (label), 24);
    GtkWidget *entry = password ? gtk_password_entry_new () : gtk_entry_new ();
    gtk_editable_set_text (GTK_EDITABLE (entry), value);
    gtk_widget_set_size_request (entry, 150, -1);
    gtk_widget_set_hexpand (entry, TRUE);
    gtk_grid_attach (grid, label, 0, row, 1, 1);
    gtk_grid_attach (grid, entry, 1, row, 1, 1);
    return entry;
}

static gboolean
quarantine_parse_integer (GtkWidget   *entry,
                          guint64      maximum,
                          guint64     *out_value)
{
    const gchar *text = gtk_editable_get_text (GTK_EDITABLE (entry));
    if (text[0] == '\0')
        return FALSE;
    for (const gchar *p = text; *p != '\0'; p++)
        if (!g_ascii_isdigit (*p))
            return FALSE;
    errno = 0;
    gchar *end = NULL;
    guint64 value = g_ascii_strtoull (text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || value > maximum)
        return FALSE;
    *out_value = value;
    return TRUE;
}

static gboolean
quarantine_set_string (json_t      *token,
                       const gchar *key,
                       const gchar *value,
                       gboolean     remove_empty)
{
    if (remove_empty && value[0] == '\0') {
        json_object_del (token, key);
        return TRUE;
    }
    return json_object_set_new (token, key, json_string (value)) == 0;
}

static json_t *
quarantine_build_repaired_token (QuarantineRepair *repair,
                                 GError          **err)
{
    json_t *token = json_is_object (repair->original)
        ? json_deep_copy (repair->original) : json_object ();
    if (token == NULL) {
        g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                             "Could not stage the repaired token.");
        return NULL;
    }
    const gchar *label = gtk_editable_get_text (GTK_EDITABLE (repair->fields[REPAIR_LABEL]));
    const gchar *issuer = gtk_editable_get_text (GTK_EDITABLE (repair->fields[REPAIR_ISSUER]));
    const gchar *group = gtk_editable_get_text (GTK_EDITABLE (repair->fields[REPAIR_GROUP]));
    const gchar *type = gtk_editable_get_text (GTK_EDITABLE (repair->fields[REPAIR_TYPE]));
    const gchar *secret = gtk_editable_get_text (GTK_EDITABLE (repair->fields[REPAIR_SECRET]));
    const gchar *algo = gtk_editable_get_text (GTK_EDITABLE (repair->fields[REPAIR_ALGORITHM]));
    if (!quarantine_set_string (token, "label", label, FALSE) ||
        !quarantine_set_string (token, "issuer", issuer, TRUE) ||
        !quarantine_set_string (token, "group", group, TRUE) ||
        !quarantine_set_string (token, "type", type, FALSE) ||
        !quarantine_set_string (token, "secret", secret, FALSE) ||
        !quarantine_set_string (token, "algo", algo, FALSE)) {
        json_decref (token);
        g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                             "Could not stage the repaired token fields.");
        return NULL;
    }

    guint64 value = 0;
    if (!quarantine_parse_integer (repair->fields[REPAIR_DIGITS], G_MAXINT64, &value))
        goto invalid_number;
    if (json_object_set_new (token, "digits", json_integer ((json_int_t) value)) != 0)
        goto no_space;
    if (g_ascii_strcasecmp (type, "TOTP") == 0) {
        if (!quarantine_parse_integer (repair->fields[REPAIR_PERIOD], G_MAXINT64, &value))
            goto invalid_number;
        if (json_object_set_new (token, "period", json_integer ((json_int_t) value)) != 0)
            goto no_space;
        json_object_del (token, "counter");
    } else if (g_ascii_strcasecmp (type, "HOTP") == 0) {
        if (!quarantine_parse_integer (repair->fields[REPAIR_COUNTER],
                                       OTP_HOTP_COUNTER_MAX - 1, &value))
            goto invalid_number;
        if (json_object_set_new (token, "counter", json_integer ((json_int_t) value)) != 0)
            goto no_space;
        json_object_del (token, "period");
    }
    return token;

invalid_number:
    json_decref (token);
    g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                         "Digits, period, and counter must be whole numbers in range.");
    return NULL;

no_space:
    json_decref (token);
    g_set_error_literal (err, G_IO_ERROR, G_IO_ERROR_NO_SPACE,
                         "Could not stage the repaired token numbers.");
    return NULL;
}

static void quarantine_repair_clicked (GtkButton *button, gpointer user_data);
static void quarantine_delete_clicked (GtkButton *button, gpointer user_data);

static void
quarantine_rebuild_list (QuarantineReview *review)
{
    while (gtk_widget_get_first_child (review->list_box) != NULL)
        gtk_box_remove (GTK_BOX (review->list_box), gtk_widget_get_first_child (review->list_box));

    json_t *tokens = review->db_data->quarantined_tokens;
    gsize count = json_is_array (tokens) ? json_array_size (tokens) : 0;
    for (guint i = 0; i < count; i++) {
        json_t *token = json_array_get (tokens, i);
        const gchar *label = json_string_value (json_object_get (token, "label"));
        const gchar *issuer = json_string_value (json_object_get (token, "issuer"));
        g_autofree gchar *title = NULL;
        if (label != NULL && label[0] != '\0')
            title = g_strdup (label);
        else if (issuer != NULL && issuer[0] != '\0')
            title = g_strdup (issuer);
        else
            title = g_strdup_printf (_("Invalid token %u"), i + 1);

        GError *validation_error = NULL;
        otp_validate_token_object (token, i, &validation_error);
        GtkWidget *row = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
        gtk_widget_add_css_class (row, "card");
        gtk_widget_set_margin_top (row, 6);
        gtk_widget_set_margin_bottom (row, 6);
        gtk_widget_set_margin_start (row, 8);
        gtk_widget_set_margin_end (row, 8);
        GtkWidget *heading = gtk_label_new (title);
        gtk_label_set_xalign (GTK_LABEL (heading), 0.0f);
        gtk_widget_add_css_class (heading, "heading");
        gtk_box_append (GTK_BOX (row), heading);
        GtkWidget *reason = gtk_label_new (validation_error ? validation_error->message : _("Token needs repair."));
        gtk_label_set_xalign (GTK_LABEL (reason), 0.0f);
        gtk_label_set_wrap (GTK_LABEL (reason), TRUE);
        gtk_box_append (GTK_BOX (row), reason);
        g_clear_error (&validation_error);

        GtkWidget *buttons = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 6);
        gtk_widget_set_halign (buttons, GTK_ALIGN_END);
        GtkWidget *repair = gtk_button_new_with_label (_("Repair…"));
        GtkWidget *remove = gtk_button_new_with_label (_("Delete"));
        gtk_widget_add_css_class (remove, "destructive-action");
        g_object_set_data (G_OBJECT (repair), "quarantine-index", GUINT_TO_POINTER (i + 1));
        g_object_set_data (G_OBJECT (remove), "quarantine-index", GUINT_TO_POINTER (i + 1));
        g_signal_connect (repair, "clicked", G_CALLBACK (quarantine_repair_clicked), review);
        g_signal_connect (remove, "clicked", G_CALLBACK (quarantine_delete_clicked), review);
        gtk_box_append (GTK_BOX (buttons), repair);
        gtk_box_append (GTK_BOX (buttons), remove);
        gtk_box_append (GTK_BOX (row), buttons);
        gtk_box_append (GTK_BOX (review->list_box), row);
    }
}

static void
quarantine_report_error (QuarantineReview *review,
                         const gchar      *message)
{
    g_autoptr (OTPClientWindow) window = g_weak_ref_get (&review->window_ref);
    if (window != NULL)
        show_error_toast (window, "%s", message);
}

static void
quarantine_delete_clicked (GtkButton *button,
                           gpointer   user_data)
{
    QuarantineReview *review = user_data;
    guint index = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "quarantine-index")) - 1;
    GError *err = NULL;
    if (!db_delete_quarantined_token (review->db_data, index, &err)) {
        quarantine_report_error (review, err ? err->message : _("Could not delete the quarantined token."));
        g_clear_error (&err);
        quarantine_rebuild_list (review);
        return;
    }
    g_autoptr (OTPClientWindow) window = g_weak_ref_get (&review->window_ref);
    if (window != NULL)
        on_db_modified (window);
    quarantine_rebuild_list (review);
    if (db_get_quarantined_count (review->db_data) == 0)
        adw_dialog_close (ADW_DIALOG (review->dialog));
}

static void
quarantine_repair_response (GObject      *source,
                            GAsyncResult *result,
                            gpointer      user_data)
{
    QuarantineRepair *repair = user_data;
    const gchar *response = adw_alert_dialog_choose_finish (
        ADW_ALERT_DIALOG (source), result);
    if (g_strcmp0 (response, "repair") != 0) {
        quarantine_repair_free (repair);
        return;
    }

    g_autoptr (AdwDialog) review_dialog = g_weak_ref_get (&repair->review_ref);
    QuarantineReview *review = review_dialog != NULL
        ? g_object_get_data (G_OBJECT (review_dialog), "quarantine-review") : NULL;
    g_autoptr (OTPClientWindow) window = review != NULL
        ? g_weak_ref_get (&review->window_ref) : NULL;
    OTPClientApplication *app = window != NULL ? OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (window))) : NULL;
    json_t *current = repair->index < db_get_quarantined_count (repair->db_data)
        ? json_array_get (repair->db_data->quarantined_tokens, repair->index) : NULL;
    if (app == NULL || otpclient_application_get_app_locked (app) ||
        otpclient_application_get_db_data (app) != repair->db_data ||
        otpclient_application_get_lock_generation (app) != repair->lock_generation ||
        current == NULL || !json_equal (current, repair->original)) {
        if (window != NULL)
            show_error_toast (window, "%s", _("The database changed. Reopen the repair form before saving."));
        quarantine_repair_free (repair);
        return;
    }

    GError *err = NULL;
    json_t *token = quarantine_build_repaired_token (repair, &err);
    if (token != NULL && !db_repair_quarantined_token (repair->db_data,
                                                        repair->index, token, &err)) {
        json_decref (token);
        token = NULL;
    }
    if (token == NULL) {
        g_autoptr (AdwAlertDialog) error_dialog = ADW_ALERT_DIALOG (
            adw_alert_dialog_new (_("Repair failed"), err ? err->message : _("Invalid token fields.")));
        adw_alert_dialog_add_response (error_dialog, "close", _("Close"));
        if (review_dialog != NULL)
            adw_dialog_present (ADW_DIALOG (error_dialog), GTK_WIDGET (review_dialog));
        g_clear_error (&err);
        quarantine_repair_free (repair);
        return;
    }
    json_decref (token);

    if (review_dialog != NULL) {
        if (review != NULL) {
            if (window != NULL)
                on_db_modified (window);
            quarantine_rebuild_list (review);
            if (db_get_quarantined_count (review->db_data) == 0)
                adw_dialog_close (review_dialog);
        }
    }
    quarantine_repair_free (repair);
}

static void
quarantine_repair_clicked (GtkButton *button,
                           gpointer   user_data)
{
    QuarantineReview *review = user_data;
    guint index = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (button), "quarantine-index")) - 1;
    json_t *original = json_array_get (review->db_data->quarantined_tokens, index);
    if (original == NULL)
        return;

    QuarantineRepair *repair = g_new0 (QuarantineRepair, 1);
    g_weak_ref_init (&repair->review_ref, review->dialog);
    repair->db_data = database_data_ref (review->db_data);
    OTPClientWindow *window = g_weak_ref_get (&review->window_ref);
    if (window != NULL) {
        OTPClientApplication *app = OTPCLIENT_APPLICATION (
            gtk_window_get_application (GTK_WINDOW (window)));
        repair->lock_generation = app != NULL
            ? otpclient_application_get_lock_generation (app) : 0;
        g_object_unref (window);
    }
    repair->index = index;
    repair->original = json_deep_copy (original);

    static const gchar *keys[] = { "label", "issuer", "group", "type", "secret", "algo", "digits", "period", "counter" };
    static const gchar *titles[] = { N_("Label"), N_("Issuer"), N_("Group"), N_("Type"), N_("Base32 secret"), N_("Algorithm"), N_("Digits"), N_("Period"), N_("Counter") };
    static const gchar *defaults[] = { "", "", "", "TOTP", "", "SHA1", "6", "30", "0" };
    GtkWidget *grid = gtk_grid_new ();
    gtk_grid_set_row_spacing (GTK_GRID (grid), 8);
    gtk_grid_set_column_spacing (GTK_GRID (grid), 12);
    for (guint i = 0; i < G_N_ELEMENTS (keys); i++) {
        g_autofree gchar *text = quarantine_field_text (original, keys[i], defaults[i]);
        repair->fields[i] = quarantine_repair_add_field (GTK_GRID (grid), i,
            _(titles[i]), text, i == REPAIR_SECRET);
    }
    GtkWidget *scrolled = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (scrolled), 320);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), grid);

    AdwAlertDialog *dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new (
        _("Repair token"), _("Correct the fields below. The repaired token is validated before it replaces the quarantined copy.")));
    adw_alert_dialog_add_responses (dialog, "cancel", _("Cancel"), "repair", _("Validate and Save"), NULL);
    adw_alert_dialog_set_response_appearance (dialog, "repair", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response (dialog, "repair");
    adw_alert_dialog_set_extra_child (dialog, scrolled);
    adw_alert_dialog_choose (dialog, GTK_WIDGET (review->dialog), NULL,
        (GAsyncReadyCallback) quarantine_repair_response, repair);
}

static void
quarantine_review_dialog_free (gpointer data)
{
    quarantine_review_free (data);
}

static void
action_review_quarantine (GtkWidget  *widget,
                          const char *action_name G_GNUC_UNUSED,
                          GVariant   *parameter G_GNUC_UNUSED)
{
    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    DatabaseData *db_data = app != NULL ? otpclient_application_get_db_data (app) : NULL;
    if (db_data == NULL || db_get_quarantined_count (db_data) == 0)
        return;

    GtkWidget *list = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *scrolled = gtk_scrolled_window_new ();
    gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_min_content_height (GTK_SCROLLED_WINDOW (scrolled), 300);
    gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), list);
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG (
        adw_alert_dialog_new (_("Tokens requiring repair"),
                              _("These entries were preserved because their OTP fields are invalid. Repair and validate an entry, or delete it explicitly.")));
    adw_alert_dialog_add_response (dialog, "close", _("Close"));
    adw_alert_dialog_set_extra_child (dialog, scrolled);

    QuarantineReview *review = g_new0 (QuarantineReview, 1);
    g_weak_ref_init (&review->window_ref, self);
    review->db_data = database_data_ref (db_data);
    review->list_box = list;
    review->dialog = dialog;
    g_object_set_data_full (G_OBJECT (dialog), "quarantine-review", review,
                            quarantine_review_dialog_free);
    quarantine_rebuild_list (review);
    adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
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
    } else if (summary->skipped_unselected > 0) {
        msg = g_strdup_printf (_("Imported %u; skipped %u duplicates, %u invalid, and %u not selected."),
                               summary->added, summary->skipped_duplicates,
                               summary->skipped_invalid, summary->skipped_unselected);
    } else if (summary->skipped_invalid > 0 && summary->skipped_duplicates > 0) {
        msg = g_strdup_printf (_("Imported %u, skipped %u duplicate and %u invalid."),
                               summary->added,
                               summary->skipped_duplicates,
                               summary->skipped_invalid);
    } else if (summary->skipped_invalid > 0) {
        msg = g_strdup_printf (_("Imported %u, skipped %u invalid."),
                               summary->added, summary->skipped_invalid);
    } else if (summary->added == 0) {
        msg = g_strdup_printf (ngettext ("Skipped %u duplicate.",
                                         "Skipped %u duplicates.",
                                         summary->skipped), summary->skipped);
    } else {
        msg = g_strdup_printf (ngettext ("Imported %u, skipped %u duplicate.",
                                         "Imported %u, skipped %u duplicates.",
                                         summary->skipped),
                               summary->added, summary->skipped);
    }

    if (summary->batch_size > 1) {
        g_autofree gchar *batch_note = g_strdup_printf (
            _("Imported batch %u of %u. Scan the remaining Google migration QR codes."),
            summary->batch_index + 1, summary->batch_size);
        gchar *combined = g_strconcat (msg, "\n", batch_note, NULL);
        g_free (msg);
        msg = combined;
    }

    if (summary->skipped_invalid > 0) {
        AdwDialog *dialog = adw_alert_dialog_new (_("Import completed with skipped entries"), msg);
        adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "close", _("Close"));
        GtkWidget *scrolled = gtk_scrolled_window_new ();
        gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrolled), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_scrolled_window_set_max_content_height (GTK_SCROLLED_WINDOW (scrolled), 250);
        gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (scrolled), TRUE);
        GtkWidget *details = gtk_label_new (summary->details);
        gtk_label_set_wrap (GTK_LABEL (details), TRUE);
        gtk_label_set_selectable (GTK_LABEL (details), TRUE);
        gtk_label_set_xalign (GTK_LABEL (details), 0);
        gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scrolled), details);
        adw_alert_dialog_set_extra_child (ADW_ALERT_DIALOG (dialog), scrolled);
        adw_dialog_present (dialog, GTK_WIDGET (self));
        return;
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

/* Shared tail for the "Scan QR from File / Webcam / Clipboard" actions.
 * Takes ownership of @otpauth_uri (frees it before returning). */
static void
add_token_from_otpauth_uri (OTPClientWindow *self,
                            gchar           *otpauth_uri)
{
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL || window_is_locked (self)) {
        sensitive_g_free (otpauth_uri);
        return;
    }

    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL) {
        sensitive_g_free (otpauth_uri);
        return;
    }

    g_autoptr (OtpImportDiagnostics) diagnostics = otp_import_diagnostics_new ();
    GSList *otps = NULL;
    guint invalid = 0;
    guint batch_size = 1;
    guint batch_index = 0;
    GError *decode_error = NULL;
    if (g_str_has_prefix (otpauth_uri, "otpauth-migration://"))
        otps = google_migration_decode_full (otpauth_uri, &invalid, &batch_size,
                                        &batch_index, diagnostics, &decode_error);
    else
        set_otps_from_uris_full (otpauth_uri, &otps, diagnostics);
    sensitive_g_free (otpauth_uri);

    if (otps == NULL) {
        show_error_toast (self, "%s",
                          decode_error != NULL ? decode_error->message
                                               : _("QR code is not an OTP URI"));
        g_clear_error (&decode_error);
        return;
    }

    GError *err = NULL;
    OtpImportReport report = {0};
    db_import_otps (db_data, otps, &report, &err);
    free_otps_gslist (otps);
    if (err != NULL) {
        show_error_toast (self, _("Failed to add scanned token: %s"), err->message);
        g_clear_error (&err);
        return;
    }
    g_autofree gchar *details = otp_import_diagnostics_format (diagnostics);
    ImportSummary summary = {
        .added = report.added,
        .skipped_duplicates = report.skipped_duplicates,
        .skipped_invalid = report.skipped_invalid + diagnostics->skipped_invalid,
        .skipped = report.skipped_duplicates + report.skipped_invalid + diagnostics->skipped_invalid,
        .batch_size = batch_size, .batch_index = batch_index, .details = details,
    };
    on_import_done (&summary, self);
}

typedef struct {
    GWeakRef window_ref;
    /* Only the open-database picker sets this: the path of the sidebar entry
     * whose location is being re-picked, or NULL for a plain open. It rides
     * along with the one picker invocation on purpose, so a cancelled chooser
     * cannot leak the relocation intent into the next open. */
    gchar *replace_path;
    gchar *backup_source;
    /* The database and lock generation that were active when the async work
     * started. A result delivered after the user switched/locked (or switched
     * away and back) must not be applied to whatever database is open now. */
    gchar *origin_db_path;
    guint  origin_generation;
} WindowAsyncContext;

static WindowAsyncContext *
window_async_context_new (OTPClientWindow *self)
{
    WindowAsyncContext *ctx = g_new0 (WindowAsyncContext, 1);
    g_weak_ref_init (&ctx->window_ref, self);

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app != NULL)
    {
        DatabaseData *db_data = otpclient_application_get_db_data (app);
        ctx->origin_db_path = g_strdup (db_data != NULL ? db_data->db_path : NULL);
        ctx->origin_generation = otpclient_application_get_lock_generation (app);
    }
    return ctx;
}

static void
window_async_context_free (WindowAsyncContext *ctx)
{
    if (ctx == NULL)
        return;
    g_weak_ref_clear (&ctx->window_ref);
    g_free (ctx->replace_path);
    g_free (ctx->backup_source);
    g_free (ctx->origin_db_path);
    g_free (ctx);
}

/* TRUE when the database this async work started for is no longer the active,
 * unlocked one: the app locked, the database was replaced (even by the same
 * path), or a different database is open. Callers use this to discard a result
 * instead of importing it into the wrong database. */
static gboolean
window_async_origin_changed (OTPClientWindow      *self,
                             WindowAsyncContext   *ctx)
{
    if (window_is_locked (self))
        return TRUE;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
        return TRUE;

    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL || db_data->db_path == NULL)
        return TRUE;

    if (g_strcmp0 (db_data->db_path, ctx->origin_db_path) != 0)
        return TRUE;

    if (otpclient_application_get_lock_generation (app) != ctx->origin_generation)
        return TRUE;

    return FALSE;
}

/* Like window_async_origin_changed(), but for work (creating or opening a
 * database) that legitimately runs with no database currently open. Only the
 * lock state and the session generation matter; a missing db_data is fine. */
static gboolean
window_async_session_changed (OTPClientWindow    *self,
                              WindowAsyncContext *ctx)
{
    if (window_is_locked (self))
        return TRUE;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL || otpclient_application_is_unlocking (app))
        return TRUE;

    return otpclient_application_get_lock_generation (app) != ctx->origin_generation;
}

static void
on_qr_file_selected (GObject      *source,
                     GAsyncResult *result,
                     gpointer      user_data)
{
    WindowAsyncContext *ctx = user_data;
    g_autoptr (OTPClientWindow) self = g_weak_ref_get (&ctx->window_ref);
    if (self == NULL) {
        window_async_context_free (ctx);
        return;
    }

    GtkFileDialog *dialog = GTK_FILE_DIALOG (source);
    GError *err = NULL;
    g_autoptr (GFile) file = gtk_file_dialog_open_finish (dialog, result, &err);
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED) || self->disposing) {
        g_clear_error (&err);
        window_async_context_free (ctx);
        return;
    }
    g_clear_error (&err);
    if (file == NULL)
    {
        window_async_context_free (ctx);
        return;
    }

    /* The user may have locked or switched database while the chooser was
     * open. Do not decode (and therefore do not import) into the wrong one. */
    if (window_async_origin_changed (self, ctx)) {
        window_async_context_free (ctx);
        return;
    }

    g_autofree gchar *path = g_file_get_path (file);
    if (path == NULL) {
        window_async_context_free (ctx);
        return;
    }

    gchar *otpauth_uri = qrcode_parse_image_file (path, &err);
    if (otpauth_uri == NULL) {
        show_error_toast (self, _("Could not read QR code: %s"), err ? err->message : _("unknown error"));
        g_clear_error (&err);
        window_async_context_free (ctx);
        return;
    }

    add_token_from_otpauth_uri (self, g_steal_pointer (&otpauth_uri));
    window_async_context_free (ctx);
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

    g_clear_object (&self->file_dialog_cancellable);
    self->file_dialog_cancellable = g_cancellable_new ();
    gtk_file_dialog_open (dialog, GTK_WINDOW (self), self->file_dialog_cancellable,
                          on_qr_file_selected, window_async_context_new (self));
    g_object_unref (dialog);
}

static void
close_webcam_progress (OTPClientWindow *self)
{
    if (self->webcam_dialog == NULL)
        return;
    GtkWindow *dialog = self->webcam_dialog;
    self->webcam_dialog = NULL;
    gtk_window_destroy (dialog);
    g_object_unref (dialog);
}

static void
on_webcam_cancel_clicked (GtkButton *button G_GNUC_UNUSED,
                          OTPClientWindow *self)
{
    if (self->webcam_cancellable != NULL)
        g_cancellable_cancel (self->webcam_cancellable);
    close_webcam_progress (self);
}

static void
on_webcam_scan_done (GObject      *source G_GNUC_UNUSED,
                     GAsyncResult *result,
                     gpointer      user_data)
{
    WindowAsyncContext *ctx = user_data;
    g_autoptr (OTPClientWindow) self = g_weak_ref_get (&ctx->window_ref);
    if (self == NULL) {
        window_async_context_free (ctx);
        return;
    }

    close_webcam_progress (self);
    g_clear_object (&self->webcam_cancellable);

    GError *err = NULL;
    g_autofree gchar *otpauth_uri = webcam_scan_qrcode_finish (result, &err);
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED) || self->disposing) {
        g_clear_error (&err);
        window_async_context_free (ctx);
        return;
    }
    if (otpauth_uri == NULL) {
        show_error_toast (self, _("Webcam scan failed: %s"),
                          err ? err->message : _("unknown error"));
        g_clear_error (&err);
        window_async_context_free (ctx);
        return;
    }

    /* The scan produced a secret; if the database changed while the webcam
     * was running, wipe it instead of importing it elsewhere. */
    if (window_async_origin_changed (self, ctx)) {
        sensitive_g_free (otpauth_uri);
        otpauth_uri = NULL;
        window_async_context_free (ctx);
        return;
    }

    add_token_from_otpauth_uri (self, g_steal_pointer (&otpauth_uri));
    window_async_context_free (ctx);
}

static void
action_add_qr_webcam (GtkWidget  *widget,
                      const char *action_name,
                      GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    if (self->webcam_cancellable != NULL) {
        show_error_toast (self, "%s", _("A webcam scan is already running."));
        return;
    }

    self->webcam_cancellable = g_cancellable_new ();

    GtkWindow *dialog = GTK_WINDOW (gtk_window_new ());
    g_object_ref_sink (dialog);
    self->webcam_dialog = dialog;
    gtk_window_set_title (dialog, _("Scan QR Code"));
    gtk_window_set_transient_for (dialog, GTK_WINDOW (self));
    gtk_window_set_modal (dialog, TRUE);
    gtk_window_set_resizable (dialog, FALSE);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_top (box, 24);
    gtk_widget_set_margin_bottom (box, 24);
    gtk_widget_set_margin_start (box, 24);
    gtk_widget_set_margin_end (box, 24);
    GtkWidget *spinner = gtk_spinner_new ();
    gtk_spinner_start (GTK_SPINNER (spinner));
    gtk_widget_set_halign (spinner, GTK_ALIGN_CENTER);
    gtk_box_append (GTK_BOX (box), spinner);
    GtkWidget *label = gtk_label_new (_("Scanning for a QR code…"));
    gtk_box_append (GTK_BOX (box), label);
    GtkWidget *cancel = gtk_button_new_with_label (_("Cancel"));
    gtk_widget_set_halign (cancel, GTK_ALIGN_CENTER);
    g_signal_connect (cancel, "clicked", G_CALLBACK (on_webcam_cancel_clicked), self);
    gtk_box_append (GTK_BOX (box), cancel);
    gtk_window_set_child (dialog, box);
    gtk_window_present (dialog);

    webcam_scan_qrcode_async (self->webcam_cancellable, on_webcam_scan_done,
                              window_async_context_new (self));
}

static void
on_clipboard_texture_received (GObject      *source,
                               GAsyncResult *result,
                               gpointer      user_data)
{
    WindowAsyncContext *ctx = user_data;
    g_autoptr (OTPClientWindow) self = g_weak_ref_get (&ctx->window_ref);
    if (self == NULL) {
        window_async_context_free (ctx);
        return;
    }

    GError *err = NULL;
    g_autoptr (GdkTexture) texture =
        gdk_clipboard_read_texture_finish (GDK_CLIPBOARD (source), result, &err);
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED) || self->disposing) {
        g_clear_error (&err);
        window_async_context_free (ctx);
        return;
    }
    if (texture == NULL) {
        show_error_toast (self, _("No image on clipboard: %s"),
                          err ? err->message : _("unknown error"));
        g_clear_error (&err);
        window_async_context_free (ctx);
        return;
    }

    /* Do not decode the image (and thus not import its secret) if the
     * database was locked or switched while the clipboard read was pending. */
    if (window_async_origin_changed (self, ctx)) {
        window_async_context_free (ctx);
        return;
    }

    gchar *otpauth_uri = qrcode_parse_texture (texture, &err);
    if (otpauth_uri == NULL) {
        show_error_toast (self, _("Could not read QR code: %s"),
                          err ? err->message : _("unknown error"));
        g_clear_error (&err);
        window_async_context_free (ctx);
        return;
    }

    add_token_from_otpauth_uri (self, otpauth_uri);
    window_async_context_free (ctx);
}

static void
action_add_qr_clipboard (GtkWidget  *widget,
                         const char *action_name,
                         GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    GdkDisplay *display = gtk_widget_get_display (widget);
    if (display == NULL) {
        show_error_toast (self, _("No display available"));
        return;
    }

    GdkClipboard *clipboard = gdk_display_get_clipboard (display);
    if (clipboard == NULL) {
        show_error_toast (self, _("Clipboard is not available"));
        return;
    }

    g_clear_object (&self->clipboard_cancellable);
    self->clipboard_cancellable = g_cancellable_new ();
    gdk_clipboard_read_texture_async (clipboard, self->clipboard_cancellable,
                                      on_clipboard_texture_received,
                                      window_async_context_new (self));
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
on_backup_tokens_save_complete (GObject      *source,
                                GAsyncResult *result,
                                gpointer      user_data)
{
    WindowAsyncContext *ctx = user_data;
    g_autoptr (OTPClientWindow) self = g_weak_ref_get (&ctx->window_ref);
    if (self == NULL) {
        window_async_context_free (ctx);
        return;
    }

    GtkFileDialog *dialog = GTK_FILE_DIALOG (source);

    GError *err = NULL;
    GFile *file = gtk_file_dialog_save_finish (dialog, result, &err);
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED) || self->disposing) {
        g_clear_error (&err);
        window_async_context_free (ctx);
        return;
    }
    if (file == NULL) {
        g_clear_error (&err);
        window_async_context_free (ctx);
        return;
    }

    g_autofree gchar *path = g_file_get_path (file);
    g_object_unref (file);

    /* g_file_get_path answers NULL for a non-native location (sftp, smb, MTP,
     * Drive); db_copy_to requires a real local path. */
    if (path == NULL) {
        show_error_toast (self, "%s",
                          _("That location is not a file on this computer. Pick a local folder instead."));
        window_async_context_free (ctx);
        return;
    }

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
    {
        window_async_context_free (ctx);
        return;
    }

    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL || db_data->db_path == NULL) {
        show_error_toast (self, "%s", _("No database is currently open."));
        window_async_context_free (ctx);
        return;
    }

    /* The database was locked or switched while the file chooser was open. Say
     * so: the user picked a filename and would otherwise see nothing happen,
     * and silently backing up a different database than the one they started
     * from would be worse. */
    if (otpclient_application_get_app_locked (app) ||
        g_strcmp0 (db_data->db_path, ctx->backup_source) != 0) {
        show_error_toast (self, "%s",
                          _("The database changed while the file chooser was open. Backup cancelled."));
        window_async_context_free (ctx);
        return;
    }

    g_autofree gchar *error_msg = db_copy_to (db_data->db_path, path);
    if (error_msg != NULL) {
        show_error_toast (self, "%s", error_msg);
        window_async_context_free (ctx);
        return;
    }

    gsettings_common_set_database_time (self->settings, OTPCLIENT_BACKUP_TIMES,
                                         ctx->backup_source, g_get_real_time () / G_USEC_PER_SEC);
    gsettings_common_set_database_time (self->settings, OTPCLIENT_BACKUP_SNOOZES,
                                         ctx->backup_source, 0);

    refresh_backup_age_banner (self);

    AdwToast *toast = adw_toast_new (_("Token database backed up"));
    adw_toast_set_timeout (toast, 5);
    adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay), toast);
    window_async_context_free (ctx);
}

static void
action_backup_tokens (GtkWidget  *widget,
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
    if (db_data == NULL || db_data->db_path == NULL) {
        show_error_toast (self, "%s", _("No database is currently open."));
        return;
    }

    GtkFileDialog *dialog = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dialog, _("Back up tokens"));

    /* Suggest <db-basename>.YYYYMMDD.bak so the user can keep generations
     * side-by-side. */
    g_autofree gchar *base = g_path_get_basename (db_data->db_path);
    g_autoptr (GDateTime) now = g_date_time_new_now_local ();
    g_autofree gchar *stamp = g_date_time_format (now, "%Y%m%d");
    g_autofree gchar *suggested = g_strdup_printf ("%s.%s.bak", base, stamp);
    gtk_file_dialog_set_initial_name (dialog, suggested);

    g_clear_object (&self->file_dialog_cancellable);
    self->file_dialog_cancellable = g_cancellable_new ();
    WindowAsyncContext *ctx = window_async_context_new (self);
    ctx->backup_source = g_strdup (db_data->db_path);
    gtk_file_dialog_save (dialog, GTK_WINDOW (self), self->file_dialog_cancellable,
                          on_backup_tokens_save_complete, ctx);
    g_object_unref (dialog);
}

static void
action_snooze_backup_banner (GtkWidget  *widget,
                             const char *action_name,
                             GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    if (self->settings == NULL)
        return;

    gint64 now = (gint64) g_get_real_time () / G_USEC_PER_SEC;
    gint64 until = now + (gint64) BACKUP_BANNER_SNOOZE_DAYS * 24 * 60 * 60;
    OTPClientApplication *app = OTPCLIENT_APPLICATION (gtk_window_get_application (GTK_WINDOW (self)));
    DatabaseData *db = app != NULL ? otpclient_application_get_db_data (app) : NULL;
    if (db != NULL)
        gsettings_common_set_database_time (self->settings, OTPCLIENT_BACKUP_SNOOZES, db->db_path, until);
    refresh_backup_age_banner (self);
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
    guint pos = selected_writable_json_index (self);
    if (pos == GTK_INVALID_LIST_POSITION)
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

void
otpclient_window_undo_latest_delete (OTPClientWindow *self)
{
    otpclient_window_undo_latest (self);
}

static void
action_delete_token (GtkWidget  *widget,
                     const char *action_name,
                     GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    otpclient_window_actions_delete (OTPCLIENT_WINDOW (widget));
}

static void
action_bulk_delete (GtkWidget *widget,
                    const char *action_name G_GNUC_UNUSED,
                    GVariant *parameter G_GNUC_UNUSED)
{
    otpclient_window_actions_bulk_delete (OTPCLIENT_WINDOW (widget));
}

static void
action_bulk_set_group (GtkWidget *widget,
                       const char *action_name G_GNUC_UNUSED,
                       GVariant *parameter G_GNUC_UNUSED)
{
    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    DatabaseData *db_data = app != NULL ? otpclient_application_get_db_data (app) : NULL;
    if (db_data == NULL)
        return;
    guint selected = gtk_drop_down_get_selected (GTK_DROP_DOWN (self->bulk_group_dropdown));
    if (selected == GTK_INVALID_LIST_POSITION || self->bulk_group_model == NULL)
        return;
    guint n = g_list_model_get_n_items (G_LIST_MODEL (self->bulk_group_model));
    if (selected >= n)
        return;
    GArray *indices = bulk_selected_indices (self, NULL);
    if (indices->len == 0) {
        g_array_unref (indices);
        return;
    }
    const gchar *group = selected == 0 ? NULL
        : gtk_string_list_get_string (self->bulk_group_model, selected);
    GError *err = NULL;
    if (!db_set_tokens_group (db_data, indices, group, &err)) {
        show_error_toast (self, _("Could not assign the selected group: %s"),
                          err != NULL ? err->message : _("database write failed"));
        g_clear_error (&err);
    } else {
        on_db_modified (self);
        AdwToast *toast = adw_toast_new (_("Group updated for selected tokens"));
        adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay), toast);
    }
    g_array_unref (indices);
}

static void
action_bulk_export (GtkWidget *widget,
                    const char *action_name G_GNUC_UNUSED,
                    GVariant *parameter G_GNUC_UNUSED)
{
    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    DatabaseData *db_data = app != NULL ? otpclient_application_get_db_data (app) : NULL;
    if (db_data == NULL)
        return;
    json_t *tokens = NULL;
    GArray *indices = bulk_selected_indices (self, &tokens);
    g_array_unref (indices);
    if (!json_is_array (tokens) || json_array_size (tokens) == 0) {
        g_clear_pointer (&tokens, json_decref);
        return;
    }
    ExportDialog *dialog = export_dialog_new_selected (db_data, GTK_WIDGET (self), tokens);
    json_decref (tokens);
    if (dialog != NULL)
        adw_dialog_present (ADW_DIALOG (dialog), GTK_WIDGET (self));
}

static void
action_show_qr (GtkWidget  *widget,
                const char *action_name,
                GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    guint pos = selected_writable_json_index (self);
    if (pos == GTK_INVALID_LIST_POSITION)
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
    sensitive_g_free (otpauth_uri);
    adw_dialog_present (ADW_DIALOG (dlg), GTK_WIDGET (self));
}

/* ---- Move token to another database ---- */

typedef struct {
    GWeakRef         window_ref;
    guint            token_pos;
    json_t          *token_json;   /* deep copy, owned */
    gchar           *target_db_path;
    /* Where the move started. The success branch removes the source entry by
     * position; verifying the database (path + lock generation) and the token
     * itself (full JSON equality) first keeps a stale submission from deleting
     * whatever now sits at that position. */
    gchar           *source_db_path;
    guint            source_generation;
} MoveTokenContext;

static void
move_token_context_free (MoveTokenContext *ctx)
{
    if (ctx == NULL)
        return;
    g_weak_ref_clear (&ctx->window_ref);
    if (ctx->token_json != NULL)
        json_decref (ctx->token_json);
    g_free (ctx->target_db_path);
    g_free (ctx->source_db_path);
    g_free (ctx);
}

static gboolean
on_move_target_password (const gchar  *current_password,
                         const gchar  *password,
                         gchar       **error_message,
                         gpointer      user_data)
{
    (void) current_password;
    /* ctx belongs to the dialog, which frees it through move_token_context_free
     * on dispose. Nothing here may free it: this callback does not run at all
     * when the user dismisses the dialog, which is exactly the case that used
     * to leak. */
    MoveTokenContext *ctx = (MoveTokenContext *) user_data;
    g_autoptr (OTPClientWindow) self = g_weak_ref_get (&ctx->window_ref);
    if (self == NULL || self->disposing || window_is_locked (self))
        return TRUE;

    if (password == NULL)
        return TRUE;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
        return TRUE;

    gint32 memlock = 0;
    set_memlock_value (&memlock);

    /* Open and decrypt the target database */
    DatabaseData *target = database_data_new (ctx->target_db_path, memlock);
    target->key = gcry_calloc_secure (strlen (password) + 1, 1);
    if (target->key == NULL) {
        database_data_free (target);
        /* FALSE keeps the dialog up for a retry, so the reason has to travel
         * with it: left NULL, the dialog's own label reads "Password was
         * rejected", which is not what happened. */
        if (error_message != NULL)
            *error_message = g_strdup (_("Secure memory is exhausted"));
        return FALSE;
    }
    memcpy (target->key, password, strlen (password) + 1);

    GError *err = NULL;
    load_db (target, &err);
    if (err != NULL)
    {
        show_error_toast (self, _("Could not open target database: %s"), err->message);
        g_clear_error (&err);
        database_data_free (target);
        return TRUE;
    }

    /* Append the token to the target database. Every staging step can fail
     * under memory pressure; if any of them does, stop here. The success
     * branch below removes the source token and persists the source
     * database, so entering it with a target save that never contained the
     * token used to delete the only copy the user had. */
    if (target->in_memory_json_data == NULL)
    {
        target->in_memory_json_data = json_array ();
        if (target->in_memory_json_data == NULL)
        {
            show_error_toast (self, "%s",
                _("Could not allocate memory to stage the token in the target database. The token was not moved."));
            database_data_free (target);
            return TRUE;
        }
    }

    if (json_array_append (target->in_memory_json_data, ctx->token_json) != 0)
    {
        show_error_toast (self, "%s",
            _("Could not stage the token in the target database. The token was not moved."));
        database_data_free (target);
        return TRUE;
    }

    update_db (target, &err);
    if (err != NULL)
    {
        show_error_toast (self, _("Could not write token to target database: %s"), err->message);
        g_clear_error (&err);
    }
    else
    {
        /* Remove the token from the current (source) database - but only if
         * it is still the database this move started from and the entry at
         * the recorded position is still the very token that was copied.
         * A dialog left open across a database switch or an edit would
         * otherwise delete a positional neighbour. If writing the source
         * back fails, restore the in-memory copy so the UI stays
         * consistent with the on-disk state - otherwise a later reload
         * would resurrect the token and the user would have a silent
         * duplicate. */
        DatabaseData *src = otpclient_application_get_db_data (app);
        json_t *current = (src != NULL && src->in_memory_json_data != NULL)
            ? json_array_get (src->in_memory_json_data, ctx->token_pos) : NULL;
        gboolean source_matches =
            src != NULL && current != NULL &&
            g_strcmp0 (src->db_path, ctx->source_db_path) == 0 &&
            otpclient_application_get_lock_generation (app) == ctx->source_generation &&
            json_equal (current, ctx->token_json);
        if (!source_matches)
        {
            show_error_toast (self, "%s",
                _("The token was copied to the target database, but the source database changed meanwhile. The token now exists in both databases; please remove the outdated copy manually."));
        }
        else
        {
            json_t *removed = json_incref (current);
            json_array_remove (src->in_memory_json_data, ctx->token_pos);
            update_db (src, &err);
            if (err != NULL)
            {
                /* update_db() restores in_memory_json_data from its committed
                 * snapshot on every failure. Do not insert removed again here:
                 * doing so added a second copy of the source token after an
                 * I/O or external-modification failure. */
                show_error_toast (self,
                    _("Could not remove the token from the source database: %s. The token now exists in both databases; please remove the duplicate manually."),
                    err->message);
                g_clear_error (&err);
            }
            json_decref (removed);
            on_db_modified (self);
        }
    }

    database_data_free (target);

    return TRUE;
}

static void
on_move_db_selected (AdwAlertDialog *dialog,
                     GAsyncResult   *result,
                     gpointer        user_data)
{
    MoveTokenContext *ctx = (MoveTokenContext *) user_data;
    const gchar *response = adw_alert_dialog_choose_finish (dialog, result);
    g_autoptr (OTPClientWindow) self = g_weak_ref_get (&ctx->window_ref);

    if (self == NULL || self->disposing || window_is_locked (self) ||
        g_strcmp0 (response, "cancel") == 0 || response == NULL)
    {
        move_token_context_free (ctx);
        return;
    }

    /* The response ID is the db_path */
    ctx->target_db_path = g_strdup (response);

    PasswordDialog *pwd_dlg = password_dialog_new_full (PASSWORD_MODE_DECRYPT,
                                                        on_move_target_password,
                                                        ctx,
                                                        (GDestroyNotify) move_token_context_free);
    adw_dialog_present (ADW_DIALOG (pwd_dlg), GTK_WIDGET (self));
}

static void
action_move_token (GtkWidget  *widget,
                   const char *action_name,
                   GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    guint pos = selected_writable_json_index (self);
    if (pos == GTK_INVALID_LIST_POSITION)
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
    g_weak_ref_init (&ctx->window_ref, self);
    ctx->token_pos = pos;
    ctx->token_json = json_deep_copy (token_obj);
    /* The deep copy is the token's ticket through the dialog round-trip; if
     * staging it failed there is nothing to move and the success path would
     * go on to delete the source token for nothing. */
    if (ctx->token_json == NULL)
    {
        g_clear_pointer (&ctx, move_token_context_free);
        show_error_toast (self, "%s",
            _("Could not allocate memory to stage the token. The token was not moved."));
        return;
    }
    ctx->source_db_path = g_strdup (db_data->db_path);
    ctx->source_generation = otpclient_application_get_lock_generation (app);

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
    if (app == NULL)
        return;

    /* Context-sensitive toolbar button: while locked it re-presents the unlock
     * dialog, otherwise it locks the database (#467). */
    if (otpclient_application_get_app_locked (app))
        lock_app_present_unlock_dialog (app);
    else
        g_action_group_activate_action (G_ACTION_GROUP (app), "lock", NULL);
}

static void
locked_unlock_button_clicked (GtkButton       *button,
                              OTPClientWindow *self)
{
    (void) button;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app != NULL)
        lock_app_present_unlock_dialog (app);
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
    guint pos = selected_writable_json_index (self);
    if (pos == GTK_INVALID_LIST_POSITION)
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
        json_object_set_new (token_obj, "group", json_string (group_name));
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
    g_idle_add_full (G_PRIORITY_DEFAULT, on_db_modified_idle,
                     g_object_ref (self), g_object_unref);
}

static void
action_remove_from_group (GtkWidget  *widget,
                          const char *action_name,
                          GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    guint pos = selected_writable_json_index (self);
    if (pos == GTK_INVALID_LIST_POSITION)
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
    g_idle_add_full (G_PRIORITY_DEFAULT, on_db_modified_idle,
                     g_object_ref (self), g_object_unref);
}

typedef struct {
    GWeakRef window_ref;
    guint token_pos;
} NewGroupContext;

static void
on_new_group_response (AdwAlertDialog  *dialog,
                       GAsyncResult    *result,
                       NewGroupContext  *ctx)
{
    const gchar *response = adw_alert_dialog_choose_finish (dialog, result);
    g_autoptr (OTPClientWindow) self = g_weak_ref_get (&ctx->window_ref);
    if (g_strcmp0 (response, "add") != 0)
    {
        g_weak_ref_clear (&ctx->window_ref);
        g_free (ctx);
        return;
    }
    if (self == NULL || self->disposing || window_is_locked (self)) {
        g_weak_ref_clear (&ctx->window_ref);
        g_free (ctx);
        return;
    }

    GtkWidget *extra = adw_alert_dialog_get_extra_child (dialog);
    const gchar *group_name = gtk_editable_get_text (GTK_EDITABLE (extra));
    if (group_name == NULL || group_name[0] == '\0')
    {
        g_weak_ref_clear (&ctx->window_ref);
        g_free (ctx);
        return;
    }

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
    {
        g_weak_ref_clear (&ctx->window_ref);
        g_free (ctx);
        return;
    }

    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL || db_data->in_memory_json_data == NULL)
    {
        g_weak_ref_clear (&ctx->window_ref);
        g_free (ctx);
        return;
    }

    json_t *token_obj = json_array_get (db_data->in_memory_json_data, ctx->token_pos);
    if (token_obj != NULL)
    {
        json_object_set_new (token_obj, "group", json_string (group_name));

        GError *err = NULL;
        update_db (db_data, &err);
        if (err != NULL)
        {
            show_error_toast (self, _("Failed to assign group: %s"), err->message);
            g_clear_error (&err);
        }
        else
        {
            on_db_modified (self);
        }
    }

    g_weak_ref_clear (&ctx->window_ref);
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
    guint pos = selected_writable_json_index (self);
    if (pos == GTK_INVALID_LIST_POSITION)
        return;

    NewGroupContext *ctx = g_new0 (NewGroupContext, 1);
    g_weak_ref_init (&ctx->window_ref, self);
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

typedef struct {
    GWeakRef window_ref;
    DatabaseData *db_data;
    guint lock_generation;
    GPtrArray *groups;
    GtkWidget *dropdown;
    GtkWidget *name_entry;
} GroupManagementContext;

static void
group_management_context_free (GroupManagementContext *ctx)
{
    g_weak_ref_clear (&ctx->window_ref);
    database_data_unref (ctx->db_data);
    g_ptr_array_unref (ctx->groups);
    g_free (ctx);
}

static void
on_manage_groups_response (AdwAlertDialog *dialog,
                           GAsyncResult *result,
                           GroupManagementContext *ctx)
{
    const gchar *response = adw_alert_dialog_choose_finish (dialog, result);
    g_autoptr (OTPClientWindow) self = g_weak_ref_get (&ctx->window_ref);
    OTPClientApplication *app = self != NULL ? OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self))) : NULL;
    if (self == NULL || app == NULL || window_is_locked (self) ||
        otpclient_application_get_db_data (app) != ctx->db_data ||
        otpclient_application_get_lock_generation (app) != ctx->lock_generation ||
        (g_strcmp0 (response, "rename") != 0 && g_strcmp0 (response, "remove") != 0)) {
        group_management_context_free (ctx);
        return;
    }
    guint selected = gtk_drop_down_get_selected (GTK_DROP_DOWN (ctx->dropdown));
    if (selected >= ctx->groups->len) {
        group_management_context_free (ctx);
        return;
    }
    const gchar *old_name = g_ptr_array_index (ctx->groups, selected);
    const gchar *new_name = gtk_editable_get_text (GTK_EDITABLE (ctx->name_entry));
    if (g_strcmp0 (response, "rename") == 0 &&
        (new_name == NULL || new_name[0] == '\0' || g_strcmp0 (old_name, new_name) == 0)) {
        group_management_context_free (ctx);
        return;
    }
    GError *err = NULL;
    gboolean success = g_strcmp0 (response, "rename") == 0
        ? db_rename_group (ctx->db_data, old_name, new_name, &err)
        : db_remove_group (ctx->db_data, old_name, &err);
    if (!success)
        show_error_toast (self, _("Could not update group: %s"),
                          err != NULL ? err->message : _("database write failed"));
    else
        on_db_modified (self);
    g_clear_error (&err);
    group_management_context_free (ctx);
}

static void
action_manage_groups (GtkWidget *widget,
                      const char *action_name G_GNUC_UNUSED,
                      GVariant *parameter G_GNUC_UNUSED)
{
    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    DatabaseData *db_data = app != NULL ? otpclient_application_get_db_data (app) : NULL;
    if (db_data == NULL || db_data->in_memory_json_data == NULL)
        return;

    GHashTable *seen = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
    gsize i;
    json_t *token;
    json_array_foreach (db_data->in_memory_json_data, i, token) {
        const gchar *group = json_string_value (json_object_get (token, "group"));
        if (group != NULL && group[0] != '\0')
            g_hash_table_add (seen, g_strdup (group));
    }
    GList *keys = g_list_sort (g_hash_table_get_keys (seen), (GCompareFunc) g_utf8_collate);
    if (keys == NULL) {
        g_hash_table_unref (seen);
        show_error_toast (self, "%s", _("There are no groups to manage."));
        return;
    }

    GroupManagementContext *ctx = g_new0 (GroupManagementContext, 1);
    g_weak_ref_init (&ctx->window_ref, self);
    ctx->db_data = database_data_ref (db_data);
    ctx->lock_generation = otpclient_application_get_lock_generation (app);
    ctx->groups = g_ptr_array_new_with_free_func (g_free);
    GtkStringList *model = gtk_string_list_new (NULL);
    for (GList *l = keys; l != NULL; l = l->next) {
        const gchar *name = l->data;
        g_ptr_array_add (ctx->groups, g_strdup (name));
        gtk_string_list_append (model, name);
    }
    g_list_free (keys);
    g_hash_table_unref (seen);

    GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 10);
    ctx->dropdown = gtk_drop_down_new (G_LIST_MODEL (model), NULL);
    ctx->name_entry = gtk_entry_new ();
    gtk_entry_set_placeholder_text (GTK_ENTRY (ctx->name_entry), _("New group name"));
    gtk_box_append (GTK_BOX (box), ctx->dropdown);
    gtk_box_append (GTK_BOX (box), ctx->name_entry);
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new (
        _("Manage Groups"), _("Rename a group across all its tokens, or remove the group from those tokens.")));
    adw_alert_dialog_add_responses (dialog, "cancel", _("Cancel"),
                                    "remove", _("Remove Group"),
                                    "rename", _("Rename Group"), NULL);
    adw_alert_dialog_set_response_appearance (dialog, "remove", ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_response_appearance (dialog, "rename", ADW_RESPONSE_SUGGESTED);
    adw_alert_dialog_set_default_response (dialog, "rename");
    adw_alert_dialog_set_extra_child (dialog, box);
    adw_alert_dialog_choose (dialog, GTK_WIDGET (self), NULL,
                             (GAsyncReadyCallback) on_manage_groups_response, ctx);
}

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

    if (window_is_locked (self))
        return;

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
                gtk_single_selection_set_selected (self->otp_selection, i);
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

    /* The row's primary action goes first. The label depends on the row, so it
     * is built here rather than sitting in context-menus.ui. No accelerator is
     * shown: Ctrl+C is scoped to a key controller on the token list, not
     * registered as a window accel, so it stays out of the way of text entries. */
    {
        const gchar *action_label = NULL;
        token_action_for_entry (OTP_ENTRY (gtk_single_selection_get_selected_item (self->otp_selection)),
                                &action_label, NULL);
        GMenu *action_section = g_menu_new ();
        g_menu_append (action_section, action_label, "win.activate-token");
        g_menu_prepend_section (menu, NULL, G_MENU_MODEL (action_section));
        g_object_unref (action_section);
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
    popup_menu_at (popover, self->otp_list, x, y);

    g_object_unref (menu);
    g_object_unref (builder);
}

static gboolean
on_new_db_password_received (const gchar  *current_password,
                              const gchar  *password,
                              gchar       **error_message,
                              gpointer      user_data);

typedef struct {
    GWeakRef window_ref;
    gchar *db_path;
    gchar *replace_path;
    /* Lock generation captured when the dialog was created. A submission that
     * lands after the session locked/switched/replaced its database is
     * permanently stale and must not reach set_db_data; see
     * on_open_db_password_received. */
    guint origin_generation;
} NewDbContext;

static void
new_db_context_free (NewDbContext *ctx)
{
    if (ctx == NULL)
        return;
    g_weak_ref_clear (&ctx->window_ref);
    g_free (ctx->db_path);
    g_free (ctx->replace_path);
    g_free (ctx);
}

static void
on_new_db_file_selected (GObject      *source,
                          GAsyncResult *result,
                          gpointer      user_data)
{
    WindowAsyncContext *async_ctx = user_data;
    g_autoptr (OTPClientWindow) self = g_weak_ref_get (&async_ctx->window_ref);
    if (self == NULL) {
        window_async_context_free (async_ctx);
        return;
    }

    GtkFileDialog *dialog = GTK_FILE_DIALOG (source);
    GError *err = NULL;
    g_autoptr (GFile) file = gtk_file_dialog_save_finish (dialog, result, &err);
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED) || self->disposing) {
        g_clear_error (&err);
        window_async_context_free (async_ctx);
        return;
    }
    g_clear_error (&err);
    if (file == NULL) {
        window_async_context_free (async_ctx);
        return;
    }

    g_autofree gchar *path = g_file_get_path (file);
    if (path == NULL) {
        window_async_context_free (async_ctx);
        return;
    }

    /* The session may have locked or switched while the chooser was open. */
    if (window_async_session_changed (self, async_ctx)) {
        window_async_context_free (async_ctx);
        return;
    }

    /* Ensure .enc extension */
    gchar *db_path;
    if (!g_str_has_suffix (path, ".enc"))
        db_path = g_strconcat (path, ".enc", NULL);
    else
        db_path = g_strdup (path);

    OTPClientApplication *ctx_app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));

    NewDbContext *ctx = g_new0 (NewDbContext, 1);
    g_weak_ref_init (&ctx->window_ref, self);
    ctx->db_path = db_path;
    ctx->origin_generation = (ctx_app != NULL)
        ? otpclient_application_get_lock_generation (ctx_app) : 0;

    PasswordDialog *pwd_dlg = password_dialog_new_full (PASSWORD_MODE_NEW,
                                                        on_new_db_password_received,
                                                        ctx,
                                                        (GDestroyNotify) new_db_context_free);
    adw_dialog_present (ADW_DIALOG (pwd_dlg), GTK_WIDGET (self));
    window_async_context_free (async_ctx);
}

static gboolean
on_new_db_password_received (const gchar  *current_password,
                              const gchar  *password,
                              gchar       **error_message,
                              gpointer      user_data)
{
    (void) current_password;
    /* Owned by the dialog, freed on its dispose. The path is stolen only once
     * every pre-flight check has passed, so a rejected submission leaves the
     * context intact and the dialog retryable. */
    NewDbContext *ctx = (NewDbContext *) user_data;
    g_autoptr (OTPClientWindow) self = g_weak_ref_get (&ctx->window_ref);

    if (self == NULL || self->disposing || window_is_locked (self) ||
        password == NULL)
        return TRUE;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
        return TRUE;

    /* A keyring-triggered unlock can still be in flight while this dialog is
     * up (a slow secret portal at startup, say). Its worker holds a raw
     * pointer to the active db_data; letting set_db_data run now would free
     * it mid-Argon2id. Return FALSE with a reason so the dialog stays open
     * and the submission can be retried once the unlock settles. */
    if (otpclient_application_is_unlocking (app))
    {
        if (error_message != NULL)
            *error_message = g_strdup (
                _("The database is still being unlocked. Please wait a moment and try again."));
        return FALSE;
    }

    /* The session moved on since the dialog was created (locked, switched, or
     * replaced its database). This context is permanently stale: reject it and
     * let the dialog close rather than install anything into the new session. */
    if (otpclient_application_get_lock_generation (app) != ctx->origin_generation)
    {
        show_error_toast (self, "%s",
                          _("The session changed while this dialog was open. Please try again."));
        return TRUE;
    }

    gchar *db_path = g_steal_pointer (&ctx->db_path);

    /* Free old db_data if present */
    DatabaseData *old_db = otpclient_application_get_db_data (app);
    if (old_db != NULL)
    {
        otpclient_window_stop_otp_timer (self);
        g_list_store_remove_all (self->otp_store);
    }

    gint32 memlock = 0;
    set_memlock_value (&memlock);

    /* Create new DatabaseData */
    DatabaseData *db_data = database_data_new (db_path, memlock);
    g_free (db_path);
    db_path = NULL;
    db_data->key = gcry_calloc_secure (strlen (password) + 1, 1);
    if (db_data->key == NULL) {
        database_data_free (db_data);
        show_error_toast (self, "%s", _("Secure memory is exhausted"));
        return TRUE;
    }
    memcpy (db_data->key, password, strlen (password) + 1);
    db_data->argon2id_iter = ARGON2ID_DEFAULT_ITER;
    db_data->argon2id_memcost = ARGON2ID_DEFAULT_MC;
    db_data->argon2id_parallelism = ARGON2ID_DEFAULT_PARAL;
    db_data->current_db_version = DB_VERSION;

    GError *err = NULL;
    update_db (db_data, &err);
    if (err != NULL)
    {
        show_error_toast (self, _("Could not create database: %s"), err->message);
        g_clear_error (&err);
        /* update_db sets in_memory_json_data = json_array() before encrypt_db
         * runs, so a partially-initialized db_data is reachable here. The
         * previous open-coded cleanup freed only key/db_path and leaked the
         * json_t array. Defer to database_data_free which handles every
         * field correctly. */
        database_data_free (db_data);
        return TRUE;
    }

    /* Reload to populate in_memory_json_data */
    load_db (db_data, &err);
    if (err != NULL)
    {
        show_error_toast (self, _("Created database but could not reload it: %s"), err->message);
        g_clear_error (&err);
    }

    if (!otpclient_application_set_db_data (app, db_data))
    {
        /* set_db_data's boundary guard refused the replacement (a worker is
         * somehow reading the current database again). db_data ownership
         * stayed with us; install nothing and say why. */
        database_data_free (db_data);
        show_error_toast (self, "%s",
                          _("The database is still being unlocked. Please wait a moment and try again."));
        return TRUE;
    }

    /* Add to sidebar. The first DB ever added is auto-set as primary by
     * otpclient_window_add_database; subsequent creations leave the
     * existing primary alone - only an explicit "Set as Primary" changes it. */
    g_autofree gchar *display_name = gui_misc_derive_db_display_name (db_data->db_path);
    otpclient_window_add_database (self, display_name, db_data->db_path);
    otpclient_window_sync_active_flag (self);
    otpclient_window_select_database (self,
        (gint) g_list_model_get_n_items (G_LIST_MODEL (self->db_store)) - 1);

    on_db_modified (self);
    otpclient_window_start_otp_timer (self);

    if (db_data->in_memory_json_data != NULL)
        otpclient_window_set_db_actions_enabled (self, TRUE);
    return TRUE;
}

static void
new_db_button_clicked (GtkButton       *button,
                       OTPClientWindow *self)
{
    (void) button;

    /* Refuse while an unlock worker is still reading the current db_data:
     * on_new_db_file_selected -> on_new_db_password_received eventually
     * calls set_db_data, which would free db_data under the worker's
     * feet. The user only loses the file dialog round-trip. */
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app != NULL && otpclient_application_is_unlocking (app))
    {
        otpclient_window_show_error_toast (self,
            _("Please wait, the database is still being unlocked."));
        return;
    }

    GtkFileDialog *dialog = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dialog, _("Create New Database"));
    gtk_file_dialog_set_initial_name (dialog, "otpclient-db.enc");

    g_clear_object (&self->file_dialog_cancellable);
    self->file_dialog_cancellable = g_cancellable_new ();
    gtk_file_dialog_save (dialog, GTK_WINDOW (self), self->file_dialog_cancellable,
                          on_new_db_file_selected, window_async_context_new (self));
    g_object_unref (dialog);
}

static void
on_open_db_file_selected (GObject      *source,
                           GAsyncResult *result,
                           gpointer      user_data);

static gboolean
on_open_db_password_received (const gchar  *current_password,
                               const gchar  *password,
                               gchar       **error_message,
                               gpointer      user_data);

static void
on_open_db_file_selected (GObject      *source,
                           GAsyncResult *result,
                           gpointer      user_data)
{
    WindowAsyncContext *async_ctx = user_data;
    g_autoptr (OTPClientWindow) self = g_weak_ref_get (&async_ctx->window_ref);
    if (self == NULL) {
        window_async_context_free (async_ctx);
        return;
    }

    GtkFileDialog *dialog = GTK_FILE_DIALOG (source);
    GError *err = NULL;
    g_autoptr (GFile) file = gtk_file_dialog_open_finish (dialog, result, &err);
    if (g_error_matches (err, G_IO_ERROR, G_IO_ERROR_CANCELLED) || self->disposing) {
        g_clear_error (&err);
        window_async_context_free (async_ctx);
        return;
    }
    g_clear_error (&err);
    if (file == NULL) {
        window_async_context_free (async_ctx);
        return;
    }

    g_autofree gchar *path = g_file_get_path (file);
    if (path == NULL) {
        window_async_context_free (async_ctx);
        return;
    }

    /* The session may have locked or switched while the chooser was open. */
    if (window_async_session_changed (self, async_ctx)) {
        window_async_context_free (async_ctx);
        return;
    }

    OTPClientApplication *ctx_app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));

    NewDbContext *ctx = g_new0 (NewDbContext, 1);
    g_weak_ref_init (&ctx->window_ref, self);
    ctx->db_path = g_strdup (path);
    ctx->replace_path = g_steal_pointer (&async_ctx->replace_path);
    ctx->origin_generation = (ctx_app != NULL)
        ? otpclient_application_get_lock_generation (ctx_app) : 0;

    PasswordDialog *pwd_dlg = password_dialog_new_full (PASSWORD_MODE_DECRYPT,
                                                        on_open_db_password_received,
                                                        ctx,
                                                        (GDestroyNotify) new_db_context_free);
    adw_dialog_present (ADW_DIALOG (pwd_dlg), GTK_WIDGET (self));
    window_async_context_free (async_ctx);
}

static gboolean
on_open_db_password_received (const gchar  *current_password,
                               const gchar  *password,
                               gchar       **error_message,
                               gpointer      user_data)
{
    (void) current_password;
    /* Owned by the dialog, freed on its dispose. The path is stolen only once
     * every pre-flight check has passed, so a rejected submission leaves the
     * context intact and the dialog retryable. */
    NewDbContext *ctx = (NewDbContext *) user_data;
    g_autoptr (OTPClientWindow) self = g_weak_ref_get (&ctx->window_ref);

    if (self == NULL || self->disposing || window_is_locked (self) ||
        password == NULL)
        return TRUE;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
        return TRUE;

    /* A keyring-triggered unlock can still be in flight while this dialog is
     * up. Its worker holds a raw pointer to the active db_data; letting
     * set_db_data run now would free it mid-Argon2id. Return FALSE with a
     * reason so the dialog stays open and the submission can be retried. */
    if (otpclient_application_is_unlocking (app))
    {
        if (error_message != NULL)
            *error_message = g_strdup (
                _("The database is still being unlocked. Please wait a moment and try again."));
        return FALSE;
    }

    /* Permanently stale: the session locked, switched, or replaced its
     * database since this dialog was created. */
    if (otpclient_application_get_lock_generation (app) != ctx->origin_generation)
    {
        show_error_toast (self, "%s",
                          _("The session changed while this dialog was open. Please try again."));
        return TRUE;
    }

    gchar *db_path = g_steal_pointer (&ctx->db_path);
    g_autofree gchar *replace_path = g_steal_pointer (&ctx->replace_path);

    /* Stop current DB */
    otpclient_window_stop_otp_timer (self);
    g_list_store_remove_all (self->otp_store);

    gint32 memlock = 0;
    set_memlock_value (&memlock);

    DatabaseData *db_data = database_data_new (db_path, memlock);
    g_free (db_path);
    db_path = NULL;
    db_data->key = gcry_calloc_secure (strlen (password) + 1, 1);
    if (db_data->key == NULL) {
        database_data_free (db_data);
        show_error_toast (self, "%s", _("Secure memory is exhausted"));
        return TRUE;
    }
    memcpy (db_data->key, password, strlen (password) + 1);

    GError *err = NULL;
    load_db (db_data, &err);
    if (err != NULL)
    {
        show_error_toast (self, _("Could not open database: %s"), err->message);
        g_clear_error (&err);
        /* load_db's v1->v2 migration path can set in_memory_json_data before
         * failing. database_data_free handles that partial state. */
        database_data_free (db_data);
        return TRUE;
    }

    if (!otpclient_application_set_db_data (app, db_data))
    {
        /* set_db_data's boundary guard refused the replacement (a worker is
         * somehow reading the current database again). db_data ownership
         * stayed with us; install nothing and say why. */
        database_data_free (db_data);
        show_error_toast (self, "%s",
                          _("The database is still being unlocked. Please wait a moment and try again."));
        return TRUE;
    }

    gboolean replaced = FALSE;
    if (replace_path != NULL) {
        replaced = gui_misc_replace_db_path (self->db_store, replace_path, db_data->db_path);
        g_autofree gchar *primary_path = gui_misc_get_db_path_from_cfg ();
        if (replaced && g_strcmp0 (primary_path, replace_path) == 0)
            gui_misc_save_db_path_to_cfg (db_data->db_path);
        /* The sidebar and the config now point at the new location; the keyring
         * still points at the old one, and it is keyed by path. Left alone, the
         * recovery would hand back the database but silently cost the user
         * their automatic unlock. */
        if (replaced)
            otpclient_application_relocate_stored_password (app, replace_path);
    }
    if (!replaced) {
        g_autofree gchar *display_name = gui_misc_derive_db_display_name (db_data->db_path);
        otpclient_window_add_database (self, display_name, db_data->db_path);
    }
    otpclient_window_sync_active_flag (self);
    gint selected_index = (gint) g_list_model_get_n_items (G_LIST_MODEL (self->db_store)) - 1;
    if (replaced) {
        guint n = g_list_model_get_n_items (G_LIST_MODEL (self->db_store));
        for (guint i = 0; i < n; i++) {
            g_autoptr (DatabaseEntry) entry = g_list_model_get_item (
                G_LIST_MODEL (self->db_store), i);
            if (g_strcmp0 (database_entry_get_path (entry), db_data->db_path) == 0) {
                selected_index = (gint) i;
                break;
            }
        }
    }
    otpclient_window_select_database (self, selected_index);

    on_db_modified (self);
    otpclient_window_start_otp_timer (self);

    if (db_data->in_memory_json_data != NULL)
        otpclient_window_set_db_actions_enabled (self, TRUE);
    return TRUE;
}

void
otpclient_window_present_open_database (OTPClientWindow *self,
                                        const gchar     *replace_path)
{
    g_return_if_fail (OTPCLIENT_IS_WINDOW (self));

    /* Same UAF guard as new_db_button_clicked: on_open_db_password_received
     * eventually calls set_db_data, which would free the in-flight db_data
     * being read by the unlock worker. */
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app != NULL && otpclient_application_is_unlocking (app))
    {
        otpclient_window_show_error_toast (self,
            _("Please wait, the database is still being unlocked."));
        return;
    }

    GtkFileDialog *dialog = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dialog, replace_path != NULL ? _("Locate Database")
                                                            : _("Open Database"));

    GtkFileFilter *filter = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter, _("OTPClient Database (*.enc)"));
    gtk_file_filter_add_pattern (filter, "*.enc");
    GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
    g_list_store_append (filters, filter);
    gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
    g_object_unref (filter);
    g_object_unref (filters);

    WindowAsyncContext *async_ctx = window_async_context_new (self);
    async_ctx->replace_path = g_strdup (replace_path);

    g_clear_object (&self->file_dialog_cancellable);
    self->file_dialog_cancellable = g_cancellable_new ();
    gtk_file_dialog_open (dialog, GTK_WINDOW (self), self->file_dialog_cancellable,
                          on_open_db_file_selected, async_ctx);
    g_object_unref (dialog);
}

static void
open_db_button_clicked (GtkButton       *button,
                        OTPClientWindow *self)
{
    (void) button;
    otpclient_window_present_open_database (self, NULL);
}

static void
action_restore_tokens (GtkWidget  *widget,
                       const char *action_name,
                       GVariant   *parameter)
{
    (void) action_name;
    (void) parameter;

    OTPClientWindow *self = OTPCLIENT_WINDOW (widget);

    /* Reuse the same picker -> password -> load_db -> add-to-sidebar pipeline
     * as the sidebar Open button. The previously active database is kept on
     * disk; the restored file becomes the active DB and is appended to the
     * sidebar so the user can switch between them. */
    GtkFileDialog *dialog = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dialog, _("Restore tokens"));

    GtkFileFilter *filter = gtk_file_filter_new ();
    gtk_file_filter_set_name (filter, _("OTPClient Database (*.enc, *.bak)"));
    gtk_file_filter_add_pattern (filter, "*.enc");
    gtk_file_filter_add_pattern (filter, "*.bak");
    GListStore *filters = g_list_store_new (GTK_TYPE_FILE_FILTER);
    g_list_store_append (filters, filter);
    gtk_file_dialog_set_filters (dialog, G_LIST_MODEL (filters));
    g_object_unref (filter);
    g_object_unref (filters);

    g_clear_object (&self->file_dialog_cancellable);
    self->file_dialog_cancellable = g_cancellable_new ();
    gtk_file_dialog_open (dialog, GTK_WINDOW (self), self->file_dialog_cancellable,
                          on_open_db_file_selected, window_async_context_new (self));
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

/* Present a context popover menu pointing at (x, y) in `over`'s coordinate
 * space. The popover is parented to `over`'s nearest non-scrolled GtkBox
 * ancestor, not to the list itself: a popover inside a GtkScrolledWindow is
 * clipped to the scrolled viewport, which made GtkPopoverMenu scroll even for a
 * handful of items (issue #465). Coordinates are translated into the anchor's
 * space so the menu still points at the clicked row. */
static void
popup_menu_at (GtkWidget *popover,
               GtkWidget *over,
               double     x,
               double     y)
{
    GtkWidget *anchor = gtk_widget_get_ancestor (over, GTK_TYPE_BOX);
    if (anchor == NULL)
        anchor = over;

    graphene_point_t pt;
    if (!gtk_widget_compute_point (over, anchor,
                                   &GRAPHENE_POINT_INIT ((float) x, (float) y), &pt))
    {
        pt.x = (float) x;
        pt.y = (float) y;
    }

    gtk_widget_set_parent (popover, anchor);
    GdkRectangle rect = { (int) pt.x, (int) pt.y, 1, 1 };
    gtk_popover_set_pointing_to (GTK_POPOVER (popover), &rect);
    g_signal_connect (popover, "closed", G_CALLBACK (on_popover_closed), NULL);
    gtk_popover_popup (GTK_POPOVER (popover));
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
    popup_menu_at (popover, self->database_list, x, y);

    g_object_unref (builder);
}

static void
on_rename_dialog_response (AdwAlertDialog  *dialog,
                           const gchar     *response,
                           OTPClientWindow *self)
{
    if (window_is_locked (self))
        return;
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

    g_signal_connect_object (dialog, "response",
                             G_CALLBACK (on_rename_dialog_response), self, 0);
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

    if (window_is_locked (self))
        return;
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
    otpclient_window_invalidate_cross_db (self);

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

    otpclient_window_sync_active_flag (self);
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

    g_signal_connect_object (dialog, "response",
                             G_CALLBACK (on_remove_dialog_response), self, 0);
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

/* The path is no longer fixed for the life of the row: relocating a database
 * rewrites it in place, keeping the row and its name. Without this the sidebar
 * goes on advertising the location the file has just been recovered from. */
static void
on_db_entry_path_changed (DatabaseEntry *entry,
                          GParamSpec    *pspec,
                          AdwActionRow  *row)
{
    (void) pspec;
    const gchar *path = database_entry_get_path (entry);
    adw_action_row_set_subtitle (row, path != NULL ? path : "");
}

/* Live-refresh the OTP-value cells when the "Hide OTPs by default" setting
 * changes. render_otp_value_label reads hide_otps at bind time; without this
 * hook, already-bound cells would stay stuck on the old value until they were
 * re-bound by a scroll or selection change. Re-emitting notify::otp-value
 * piggybacks on the existing bind-time listener that re-renders the label. */
static void
refresh_otp_value_cells (OTPClientWindow *self)
{
    if (self->otp_store != NULL)
    {
        guint n = g_list_model_get_n_items (G_LIST_MODEL (self->otp_store));
        for (guint i = 0; i < n; i++)
        {
            g_autoptr (OTPEntry) entry = g_list_model_get_item (G_LIST_MODEL (self->otp_store), i);
            if (entry != NULL)
                g_object_notify (G_OBJECT (entry), "otp-value");
        }
    }
    if (self->cross_db_store != NULL)
    {
        guint n = g_list_model_get_n_items (G_LIST_MODEL (self->cross_db_store));
        for (guint i = 0; i < n; i++)
        {
            g_autoptr (OTPEntry) entry = g_list_model_get_item (G_LIST_MODEL (self->cross_db_store), i);
            if (entry != NULL)
                g_object_notify (G_OBJECT (entry), "otp-value");
        }
    }
}

static void
on_hide_otps_changed (GObject    *gobject,
                      GParamSpec *pspec,
                      gpointer    user_data)
{
    (void) gobject;
    (void) pspec;
    refresh_otp_value_cells (OTPCLIENT_WINDOW (user_data));
}

/* GtkWindow's "application" is a plain property, not a construct one, so
 * g_object_new applies it after constructed() has already returned: reading it
 * from there hands back NULL and the hook below was never connected at all.
 * Hook it from the constructor, where the application is in hand either way.
 * g_signal_connect_object auto-disconnects when the window is finalized. */
static void
watch_hide_otps_setting (OTPClientWindow      *self,
                         OTPClientApplication *app)
{
    if (app == NULL)
        return;

    g_signal_connect_object (app, "notify::hide-otps",
                             G_CALLBACK (on_hide_otps_changed), self,
                             G_CONNECT_DEFAULT);
}

static void
otpclient_window_init (OTPClientWindow *self)
{
    GSettingsSchemaSource *schema_source = NULL;
    g_autoptr (GSettingsSchema) schema = NULL;

    gtk_widget_init_template (GTK_WIDGET(self));

    g_weak_ref_init (&self->clipboard_owner_entry, NULL);
    self->cross_db_monitors = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                      g_free, g_object_unref);
    self->bulk_selected_entries = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                                        g_object_unref, NULL);
    g_signal_connect (self->bulk_mode_button, "toggled",
                      G_CALLBACK (bulk_mode_toggled), self);
    GdkDisplay *display = gdk_display_get_default ();
    if (display != NULL)
        g_signal_connect_object (gdk_display_get_clipboard (display), "changed",
                                 G_CALLBACK (on_clipboard_changed), self, 0);

    GtkWidget *spinner;
#if ADW_CHECK_VERSION(1, 6, 0)
    spinner = adw_spinner_new ();
#else
    spinner = gtk_spinner_new ();
    gtk_spinner_start (GTK_SPINNER (spinner));
#endif
    gtk_widget_set_halign (spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_valign (spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request (spinner, 48, 48);
    adw_status_page_set_child (ADW_STATUS_PAGE (self->loading_status_page), spinner);

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

        /* Banner state depends on these keys - refresh whenever they change
         * (e.g. when the export dialog records a fresh export, or the user
         * snoozes the reminder). */
        g_signal_connect_swapped (self->settings, "changed::database-backup-times",
                                   G_CALLBACK (refresh_backup_age_banner), self);
        g_signal_connect_swapped (self->settings, "changed::database-backup-snoozes",
                                   G_CALLBACK (refresh_backup_age_banner), self);
        g_signal_connect (self->settings, "changed::show-sidebar",
                          G_CALLBACK (on_show_sidebar_key_changed), self);
    }
    else
    {
        g_warning ("Settings schema 'com.github.paolostivanin.OTPClient' is not installed.");
    }

    setup_otp_view (self);
    setup_database_list (self);
    update_empty_state (self);

    /* No DB is loaded yet - keep DB-dependent actions (and the empty-state
     * Add/Import CTAs that target them) disabled until populate runs. */
    otpclient_window_set_db_actions_enabled (self, FALSE);

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
    g_signal_connect (self->search_entry, "activate", G_CALLBACK (search_entry_activate), self);
g_signal_connect (self->lock_button, "clicked", G_CALLBACK (lock_button_clicked), self);
    g_signal_connect (self->new_db_button, "clicked", G_CALLBACK (new_db_button_clicked), self);
    g_signal_connect (self->open_db_button, "clicked", G_CALLBACK (open_db_button_clicked), self);
    g_signal_connect (self->no_db_create_button, "clicked", G_CALLBACK (new_db_button_clicked), self);
    g_signal_connect (self->no_db_open_button,   "clicked", G_CALLBACK (open_db_button_clicked), self);
    g_signal_connect (self->locked_unlock_button, "clicked", G_CALLBACK (locked_unlock_button_clicked), self);

    setup_dnd (self);

    /* Right-click context menu on token list */
    GtkGesture *right_click = gtk_gesture_click_new ();
    gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (right_click), GDK_BUTTON_SECONDARY);
    g_signal_connect (right_click, "pressed", G_CALLBACK (on_token_right_click), self);
    gtk_widget_add_controller (self->otp_list, GTK_EVENT_CONTROLLER (right_click));

    GtkEventController *token_keys = gtk_event_controller_key_new ();
    gtk_event_controller_set_propagation_phase (token_keys, GTK_PHASE_CAPTURE);
    g_signal_connect (token_keys, "key-pressed", G_CALLBACK (on_token_key_pressed), self);
    gtk_widget_add_controller (self->otp_list, token_keys);

    /* Double-click a row for the same copy-or-generate the Action button does */
    g_signal_connect (self->otp_list, "activate", G_CALLBACK (on_token_row_activated), self);

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
    G_OBJECT_CLASS(klass)->finalize = otpclient_window_finalize;

    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    gtk_widget_class_set_template_from_resource (widget_class, "/com/github/paolostivanin/OTPClient/ui/window.ui");
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, toast_overlay);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, add_button);
gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, search_bar);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, search_entry);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, lock_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, settings_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, backup_age_banner);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, sidebar_toggle_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, split_view);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, database_list);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, new_db_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, open_db_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, no_db_create_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, no_db_open_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, empty_state_add_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, search_empty_clear_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, otp_list);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, content_stack);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, loading_status_page);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, locked_status_page);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, locked_unlock_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, group_dropdown);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, bulk_mode_button);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, bulk_toolbar);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, bulk_count_label);
    gtk_widget_class_bind_template_child (widget_class, OTPClientWindow, bulk_group_dropdown);

    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_q, GDK_CONTROL_MASK, "app.quit", NULL);

    gtk_widget_class_install_action (widget_class, "window.search", NULL, (GtkWidgetActionActivateFunc)search_func);
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_f, GDK_CONTROL_MASK, "window.search", NULL);
    gtk_widget_class_install_action (widget_class, "win.clear-search", NULL, action_clear_search);

    gtk_widget_class_install_action (widget_class, "win.hide-and-unselect", NULL, action_hide_and_unselect);
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_h, GDK_ALT_MASK, "win.hide-and-unselect", NULL);

    /* Token actions */
    gtk_widget_class_install_action (widget_class, "win.add-manual", NULL, action_add_manual);
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_n, GDK_CONTROL_MASK, "win.add-manual", NULL);
    gtk_widget_class_install_action (widget_class, "win.add-qr-file", NULL, action_add_qr_file);
    gtk_widget_class_install_action (widget_class, "win.add-qr-webcam", NULL, action_add_qr_webcam);
    gtk_widget_class_install_action (widget_class, "win.add-qr-clipboard", NULL, action_add_qr_clipboard);
    gtk_widget_class_install_action (widget_class, "win.import", NULL, action_import);
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_i, GDK_CONTROL_MASK, "win.import", NULL);
    gtk_widget_class_install_action (widget_class, "win.export", NULL, action_export);
    gtk_widget_class_install_action (widget_class, "win.backup-tokens", NULL, action_backup_tokens);
    gtk_widget_class_install_action (widget_class, "win.restore-tokens", NULL, action_restore_tokens);
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_e, GDK_CONTROL_MASK, "win.export", NULL);
    gtk_widget_class_install_action (widget_class, "win.snooze-backup-banner", NULL, action_snooze_backup_banner);
    gtk_widget_class_install_action (widget_class, "win.review-quarantine", NULL, action_review_quarantine);
    gtk_widget_class_install_action (widget_class, "win.settings", NULL, action_settings);
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_comma, GDK_CONTROL_MASK, "win.settings", NULL);
    gtk_widget_class_install_action (widget_class, "win.activate-token", NULL, action_activate_token);
    gtk_widget_class_install_action (widget_class, "win.edit-token", NULL, action_edit_token);
    gtk_widget_class_add_binding_action (widget_class, GDK_KEY_F2, 0, "win.edit-token", NULL);
    gtk_widget_class_install_action (widget_class, "win.delete-token", NULL, action_delete_token);
    gtk_widget_class_install_action (widget_class, "win.bulk-delete", NULL, action_bulk_delete);
    gtk_widget_class_install_action (widget_class, "win.bulk-set-group", NULL, action_bulk_set_group);
    gtk_widget_class_install_action (widget_class, "win.bulk-export", NULL, action_bulk_export);
    gtk_widget_class_install_action (widget_class, "win.show-qr", NULL, action_show_qr);
    gtk_widget_class_install_action (widget_class, "win.move-token", NULL, action_move_token);
    gtk_widget_class_install_action (widget_class, "win.set-group", "s", action_set_group);
    gtk_widget_class_install_action (widget_class, "win.new-group", NULL, action_new_group);
    gtk_widget_class_install_action (widget_class, "win.manage-groups", NULL, action_manage_groups);
    gtk_widget_class_install_action (widget_class, "win.remove-from-group", NULL, action_remove_from_group);
    gtk_widget_class_install_action (widget_class, "win.rename-db", NULL, action_rename_db);
    gtk_widget_class_install_action (widget_class, "win.set-primary-db", NULL, action_set_primary_db);
    gtk_widget_class_install_action (widget_class, "win.remove-db", NULL, action_remove_db);

    gtk_widget_class_bind_template_callback (klass, search_text_changed);
}

GtkWidget *
otpclient_window_new (OTPClientApplication *application)
{
    OTPClientWindow *self = g_object_new (OTPCLIENT_TYPE_WINDOW,
                                          "application", application, NULL);
    watch_hide_otps_setting (self, application);
    return GTK_WIDGET (self);
}
