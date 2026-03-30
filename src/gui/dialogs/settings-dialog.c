#include <glib/gi18n.h>
#include "settings-dialog.h"
#include "gui-misc.h"
#include "common.h"
#include "settings-import-export.h"

struct _SettingsDialog
{
    AdwPreferencesDialog parent;

    OTPClientApplication *app;

    GtkWidget *show_next_otp_switch;
    GtkWidget *show_validity_seconds_switch;
    GtkWidget *validity_color_row;
    GtkWidget *validity_warning_color_row;
    GtkWidget *dark_theme_switch;
    GtkWidget *disable_notifications_switch;
    GtkWidget *auto_lock_switch;
    GtkWidget *inactivity_combo;
    GtkWidget *secret_service_switch;
    GtkWidget *search_provider_switch;
#ifdef ENABLE_MINIMIZE_TO_TRAY
    GtkWidget *minimize_to_tray_switch;
#endif
};

G_DEFINE_FINAL_TYPE (SettingsDialog, settings_dialog, ADW_TYPE_PREFERENCES_DIALOG)

static void
on_show_next_otp_toggled (GObject        *obj,
                          GParamSpec     *pspec,
                          SettingsDialog *self)
{
    (void) pspec;
    gboolean active = adw_switch_row_get_active (ADW_SWITCH_ROW (obj));
    otpclient_application_set_show_next_otp (self->app, active);
}

static void
on_dark_theme_toggled (GObject        *obj,
                       GParamSpec     *pspec,
                       SettingsDialog *self)
{
    (void) pspec;
    gboolean active = adw_switch_row_get_active (ADW_SWITCH_ROW (obj));
    otpclient_application_set_use_dark_theme (self->app, active);

    AdwStyleManager *style_manager = adw_style_manager_get_default ();
    adw_style_manager_set_color_scheme (style_manager,
                                        active ? ADW_COLOR_SCHEME_FORCE_DARK
                                               : ADW_COLOR_SCHEME_DEFAULT);
}

static void
on_disable_notifications_toggled (GObject        *obj,
                                  GParamSpec     *pspec,
                                  SettingsDialog *self)
{
    (void) pspec;
    gboolean active = adw_switch_row_get_active (ADW_SWITCH_ROW (obj));
    otpclient_application_set_disable_notifications (self->app, active);
}

static void
on_auto_lock_toggled (GObject        *obj,
                      GParamSpec     *pspec,
                      SettingsDialog *self)
{
    (void) pspec;
    gboolean active = adw_switch_row_get_active (ADW_SWITCH_ROW (obj));
    otpclient_application_set_auto_lock (self->app, active);
    gtk_widget_set_sensitive (self->inactivity_combo, active);
}

static void
on_inactivity_changed (AdwComboRow    *combo,
                       GParamSpec     *pspec,
                       SettingsDialog *self)
{
    (void) pspec;
    static const gint timeout_values[] = { 60, 120, 300, 600, 900, 1800, 3600 };
    guint selected = adw_combo_row_get_selected (combo);
    if (selected < G_N_ELEMENTS (timeout_values))
        otpclient_application_set_inactivity_timeout (self->app, timeout_values[selected]);
}

static void
on_secret_service_toggled (GObject        *obj,
                           GParamSpec     *pspec,
                           SettingsDialog *self)
{
    (void) pspec;
    gboolean active = adw_switch_row_get_active (ADW_SWITCH_ROW (obj));
    otpclient_application_set_use_secret_service (self->app, active);
}

static void
on_search_provider_toggled (GObject        *obj,
                            GParamSpec     *pspec,
                            SettingsDialog *self)
{
    (void) pspec;
    gboolean active = adw_switch_row_get_active (ADW_SWITCH_ROW (obj));
    otpclient_application_set_search_provider_enabled (self->app, active);
}

#ifdef ENABLE_MINIMIZE_TO_TRAY
static void
on_minimize_to_tray_toggled (GObject        *obj,
                              GParamSpec     *pspec,
                              SettingsDialog *self)
{
    (void) pspec;
    gboolean active = adw_switch_row_get_active (ADW_SWITCH_ROW (obj));
    otpclient_application_set_minimize_to_tray (self->app, active);
}
#endif

static void
on_show_validity_seconds_toggled (GObject        *obj,
                                   GParamSpec     *pspec,
                                   SettingsDialog *self)
{
    (void) pspec;
    gboolean active = adw_switch_row_get_active (ADW_SWITCH_ROW (obj));
    otpclient_application_set_show_validity_seconds (self->app, active);
    gtk_widget_set_visible (self->validity_color_row, !active);
    gtk_widget_set_visible (self->validity_warning_color_row, !active);
}

static void
on_validity_color_changed (GtkColorDialogButton *button,
                           GParamSpec           *pspec,
                           SettingsDialog       *self)
{
    (void) pspec;
    const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba (button);
    g_autofree gchar *hex = g_strdup_printf ("#%02x%02x%02x",
                                              (guint)(rgba->red * 255),
                                              (guint)(rgba->green * 255),
                                              (guint)(rgba->blue * 255));
    otpclient_application_set_validity_color (self->app, hex);
}

static void
on_validity_warning_color_changed (GtkColorDialogButton *button,
                                   GParamSpec           *pspec,
                                   SettingsDialog       *self)
{
    (void) pspec;
    const GdkRGBA *rgba = gtk_color_dialog_button_get_rgba (button);
    g_autofree gchar *hex = g_strdup_printf ("#%02x%02x%02x",
                                              (guint)(rgba->red * 255),
                                              (guint)(rgba->green * 255),
                                              (guint)(rgba->blue * 255));
    otpclient_application_set_validity_warning_color (self->app, hex);
}

static void
on_export_file_save_complete (GObject      *source,
                              GAsyncResult *result,
                              gpointer      user_data)
{
    GtkFileDialog *dialog = GTK_FILE_DIALOG (source);
    SettingsDialog *self = SETTINGS_DIALOG (user_data);
    GError *err = NULL;

    GFile *file = gtk_file_dialog_save_finish (dialog, result, &err);
    if (file == NULL) {
        if (!g_error_matches (err, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
            g_printerr ("Export settings error: %s\n", err->message);
        g_clear_error (&err);
        return;
    }

    g_autofree gchar *path = g_file_get_path (file);
    g_object_unref (file);

    gchar *json = export_settings_to_json (&err);
    if (json == NULL) {
        AdwAlertDialog *alert = ADW_ALERT_DIALOG (adw_alert_dialog_new (_("Export Failed"), err->message));
        adw_alert_dialog_add_response (alert, "ok", _("OK"));
        adw_dialog_present (ADW_DIALOG (alert), GTK_WIDGET (self));
        g_clear_error (&err);
        return;
    }

    if (!g_file_set_contents (path, json, -1, &err)) {
        AdwAlertDialog *alert = ADW_ALERT_DIALOG (adw_alert_dialog_new (_("Export Failed"), err->message));
        adw_alert_dialog_add_response (alert, "ok", _("OK"));
        adw_dialog_present (ADW_DIALOG (alert), GTK_WIDGET (self));
        g_clear_error (&err);
    }

    free (json);
}

static void
on_export_settings_clicked (GtkWidget      *button __attribute__((unused)),
                            SettingsDialog *self)
{
    GtkFileDialog *dialog = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dialog, _("Export Settings"));
    gtk_file_dialog_set_initial_name (dialog, "otpclient-settings.json");
    gtk_file_dialog_save (dialog,
                          GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self))),
                          NULL,
                          on_export_file_save_complete,
                          self);
    g_object_unref (dialog);
}

static void
on_import_file_open_complete (GObject      *source,
                              GAsyncResult *result,
                              gpointer      user_data)
{
    GtkFileDialog *dialog = GTK_FILE_DIALOG (source);
    SettingsDialog *self = SETTINGS_DIALOG (user_data);
    GError *err = NULL;

    GFile *file = gtk_file_dialog_open_finish (dialog, result, &err);
    if (file == NULL) {
        if (!g_error_matches (err, GTK_DIALOG_ERROR, GTK_DIALOG_ERROR_DISMISSED))
            g_printerr ("Import settings error: %s\n", err->message);
        g_clear_error (&err);
        return;
    }

    g_autofree gchar *path = g_file_get_path (file);
    g_object_unref (file);

    gchar *contents = NULL;
    if (!g_file_get_contents (path, &contents, NULL, &err)) {
        AdwAlertDialog *alert = ADW_ALERT_DIALOG (adw_alert_dialog_new (_("Import Failed"), err->message));
        adw_alert_dialog_add_response (alert, "ok", _("OK"));
        adw_dialog_present (ADW_DIALOG (alert), GTK_WIDGET (self));
        g_clear_error (&err);
        return;
    }

    if (!import_settings_from_json (contents, &err)) {
        AdwAlertDialog *alert = ADW_ALERT_DIALOG (adw_alert_dialog_new (_("Import Failed"), err->message));
        adw_alert_dialog_add_response (alert, "ok", _("OK"));
        adw_dialog_present (ADW_DIALOG (alert), GTK_WIDGET (self));
        g_clear_error (&err);
        g_free (contents);
        return;
    }
    g_free (contents);

    /* Sync in-memory app state from GSettings and close the dialog so
     * the user sees fresh values when they reopen it. */
    otpclient_application_reload_settings (self->app);
    adw_dialog_close (ADW_DIALOG (self));
}

static void
on_import_settings_clicked (GtkWidget      *button __attribute__((unused)),
                            SettingsDialog *self)
{
    GtkFileDialog *dialog = gtk_file_dialog_new ();
    gtk_file_dialog_set_title (dialog, _("Import Settings"));
    gtk_file_dialog_open (dialog,
                          GTK_WINDOW (gtk_widget_get_root (GTK_WIDGET (self))),
                          NULL,
                          on_import_file_open_complete,
                          self);
    g_object_unref (dialog);
}

static void
settings_dialog_init (SettingsDialog *self)
{
    (void) self;
}

static void
settings_dialog_class_init (SettingsDialogClass *klass)
{
    (void) klass;
}

SettingsDialog *
settings_dialog_new (OTPClientApplication *app)
{
    SettingsDialog *self = g_object_new (SETTINGS_TYPE_DIALOG, NULL);
    self->app = app;

    /* Display group */
    AdwPreferencesGroup *display_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (display_group, _("Display"));

    self->show_next_otp_switch = adw_switch_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->show_next_otp_switch), _("Show Next OTP"));
    adw_switch_row_set_active (ADW_SWITCH_ROW (self->show_next_otp_switch),
                               otpclient_application_get_show_next_otp (app));
    g_signal_connect (self->show_next_otp_switch, "notify::active",
                      G_CALLBACK (on_show_next_otp_toggled), self);
    adw_preferences_group_add (display_group, self->show_next_otp_switch);

    self->show_validity_seconds_switch = adw_switch_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->show_validity_seconds_switch),
                                    _("Show Validity in seconds"));
    gboolean validity_seconds_active = otpclient_application_get_show_validity_seconds (app);
    adw_switch_row_set_active (ADW_SWITCH_ROW (self->show_validity_seconds_switch),
                               validity_seconds_active);
    g_signal_connect (self->show_validity_seconds_switch, "notify::active",
                      G_CALLBACK (on_show_validity_seconds_toggled), self);
    adw_preferences_group_add (display_group, self->show_validity_seconds_switch);

    /* Countdown color picker */
    GdkRGBA validity_rgba;
    gdk_rgba_parse (&validity_rgba, otpclient_application_get_validity_color (app));
    GtkColorDialog *color_dialog = gtk_color_dialog_new ();
    GtkWidget *color_button = gtk_color_dialog_button_new (color_dialog);
    gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (color_button), &validity_rgba);
    g_signal_connect (color_button, "notify::rgba",
                      G_CALLBACK (on_validity_color_changed), self);
    self->validity_color_row = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->validity_color_row),
                                    _("Countdown Color"));
    adw_action_row_add_suffix (ADW_ACTION_ROW (self->validity_color_row), color_button);
    gtk_widget_set_visible (self->validity_color_row, !validity_seconds_active);
    adw_preferences_group_add (display_group, self->validity_color_row);

    /* Countdown warning color picker */
    GdkRGBA warning_rgba;
    gdk_rgba_parse (&warning_rgba, otpclient_application_get_validity_warning_color (app));
    GtkColorDialog *warning_color_dialog = gtk_color_dialog_new ();
    GtkWidget *warning_color_button = gtk_color_dialog_button_new (warning_color_dialog);
    gtk_color_dialog_button_set_rgba (GTK_COLOR_DIALOG_BUTTON (warning_color_button), &warning_rgba);
    g_signal_connect (warning_color_button, "notify::rgba",
                      G_CALLBACK (on_validity_warning_color_changed), self);
    self->validity_warning_color_row = adw_action_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->validity_warning_color_row),
                                    _("Countdown Warning Color"));
    adw_action_row_add_suffix (ADW_ACTION_ROW (self->validity_warning_color_row), warning_color_button);
    gtk_widget_set_visible (self->validity_warning_color_row, !validity_seconds_active);
    adw_preferences_group_add (display_group, self->validity_warning_color_row);

    self->dark_theme_switch = adw_switch_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->dark_theme_switch), _("Dark Theme"));
    adw_switch_row_set_active (ADW_SWITCH_ROW (self->dark_theme_switch),
                               otpclient_application_get_use_dark_theme (app));
    g_signal_connect (self->dark_theme_switch, "notify::active",
                      G_CALLBACK (on_dark_theme_toggled), self);
    adw_preferences_group_add (display_group, self->dark_theme_switch);

    /* Notifications group */
    AdwPreferencesGroup *notif_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (notif_group, _("Notifications"));

    self->disable_notifications_switch = adw_switch_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->disable_notifications_switch),
                                    _("Disable Notifications"));
    adw_switch_row_set_active (ADW_SWITCH_ROW (self->disable_notifications_switch),
                               otpclient_application_get_disable_notifications (app));
    g_signal_connect (self->disable_notifications_switch, "notify::active",
                      G_CALLBACK (on_disable_notifications_toggled), self);
    adw_preferences_group_add (notif_group, self->disable_notifications_switch);

    /* Security group */
    AdwPreferencesGroup *security_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (security_group, _("Security"));

    self->auto_lock_switch = adw_switch_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->auto_lock_switch), _("Auto-Lock"));
    adw_switch_row_set_active (ADW_SWITCH_ROW (self->auto_lock_switch),
                               otpclient_application_get_auto_lock (app));
    g_signal_connect (self->auto_lock_switch, "notify::active",
                      G_CALLBACK (on_auto_lock_toggled), self);
    adw_preferences_group_add (security_group, self->auto_lock_switch);

    const char * const timeout_items[] = {
        "1 minute", "2 minutes", "5 minutes", "10 minutes",
        "15 minutes", "30 minutes", "1 hour", NULL
    };
    GtkStringList *timeout_model = gtk_string_list_new (timeout_items);
    self->inactivity_combo = adw_combo_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->inactivity_combo),
                                    _("Inactivity Timeout"));
    adw_combo_row_set_model (ADW_COMBO_ROW (self->inactivity_combo), G_LIST_MODEL (timeout_model));
    gtk_widget_set_sensitive (self->inactivity_combo,
                              otpclient_application_get_auto_lock (app));
    g_signal_connect (self->inactivity_combo, "notify::selected",
                      G_CALLBACK (on_inactivity_changed), self);
    adw_preferences_group_add (security_group, self->inactivity_combo);

    self->secret_service_switch = adw_switch_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->secret_service_switch),
                                    _("Use Secret Service"));
    adw_action_row_set_subtitle (ADW_ACTION_ROW (self->secret_service_switch),
                                  _("Store the database password in the system keyring so the app unlocks automatically after login"));
    adw_switch_row_set_active (ADW_SWITCH_ROW (self->secret_service_switch),
                               otpclient_application_get_use_secret_service (app));
    g_signal_connect (self->secret_service_switch, "notify::active",
                      G_CALLBACK (on_secret_service_toggled), self);
    adw_preferences_group_add (security_group, self->secret_service_switch);

    /* Integration group */
    AdwPreferencesGroup *integration_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (integration_group, _("Integration"));

    self->search_provider_switch = adw_switch_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->search_provider_switch),
                                    _("Search Provider"));
    adw_switch_row_set_active (ADW_SWITCH_ROW (self->search_provider_switch),
                               otpclient_application_get_search_provider_enabled (app));
    g_signal_connect (self->search_provider_switch, "notify::active",
                      G_CALLBACK (on_search_provider_toggled), self);
    adw_preferences_group_add (integration_group, self->search_provider_switch);

#ifdef ENABLE_MINIMIZE_TO_TRAY
    self->minimize_to_tray_switch = adw_switch_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (self->minimize_to_tray_switch),
                                    _("Minimize to Tray"));
    adw_switch_row_set_active (ADW_SWITCH_ROW (self->minimize_to_tray_switch),
                               otpclient_application_get_minimize_to_tray (app));
    g_signal_connect (self->minimize_to_tray_switch, "notify::active",
                      G_CALLBACK (on_minimize_to_tray_toggled), self);
    adw_preferences_group_add (integration_group, self->minimize_to_tray_switch);
#endif

    /* Backup group */
    AdwPreferencesGroup *backup_group = ADW_PREFERENCES_GROUP (adw_preferences_group_new ());
    adw_preferences_group_set_title (backup_group, _("Backup"));

    GtkWidget *export_row = adw_button_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (export_row), _("Export Settings"));
    g_signal_connect (export_row, "activated",
                      G_CALLBACK (on_export_settings_clicked), self);
    adw_preferences_group_add (backup_group, export_row);

    GtkWidget *import_row = adw_button_row_new ();
    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (import_row), _("Import Settings"));
    g_signal_connect (import_row, "activated",
                      G_CALLBACK (on_import_settings_clicked), self);
    adw_preferences_group_add (backup_group, import_row);

    /* Add page */
    AdwPreferencesPage *page = ADW_PREFERENCES_PAGE (adw_preferences_page_new ());
    adw_preferences_page_set_title (page, _("Settings"));
    adw_preferences_page_set_icon_name (page, "preferences-system-symbolic");
    adw_preferences_page_add (page, display_group);
    adw_preferences_page_add (page, notif_group);
    adw_preferences_page_add (page, security_group);
    adw_preferences_page_add (page, integration_group);
    adw_preferences_page_add (page, backup_group);

    adw_preferences_dialog_add (ADW_PREFERENCES_DIALOG (self), page);

    return self;
}
