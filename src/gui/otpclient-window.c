#include <gtk/gtk.h>
#include <gcrypt.h>
#include <jansson.h>
#include "otpclient-window.h"
#include "otpclient-application.h"
#include "otpclient.h"
#include "../common/macros.h"
#include "../common/import-export.h"
#include "gui-misc.h"
#include "lock-app.h"
#include "change-db-cb.h"
#include "new-db-cb.h"
#include "change-pwd-cb.h"
#include "settings-cb.h"
#include "shortcuts-cb.h"
#include "webcam-add-cb.h"
#include "manual-add-cb.h"
#include "dbinfo-cb.h"
#include "change-db-sec.h"
#include "config-misc.h"
#include "treeview.h"
#include "get-builder.h"
#ifdef ENABLE_MINIMIZE_TO_TRAY
#include "tray.h"
#endif

static gboolean   key_pressed_cb        (GtkEventControllerKey *controller,
                                          guint                  keyval,
                                          guint                  keycode,
                                          GdkModifierType        state,
                                          gpointer               user_data);

static void       reorder_rows_cb       (GtkToggleButton       *btn,
                                          gpointer               user_data);

static gboolean   configure_event_cb    (GtkWidget             *window,
                                          GdkEventConfigure     *event,
                                          gpointer               user_data);

struct _OtpclientWindow {
    GtkApplicationWindow  parent_instance;

    /* All three builders are owned here; app_data holds non-owning references */
    GtkBuilder *builder;
    GtkBuilder *add_popover_builder;
    GtkBuilder *settings_popover_builder;
};

G_DEFINE_TYPE (OtpclientWindow, otpclient_window, GTK_TYPE_APPLICATION_WINDOW)

static void
otpclient_window_dispose (GObject *object)
{
    OtpclientWindow *self = OTPCLIENT_WINDOW (object);

    g_clear_object (&self->builder);
    g_clear_object (&self->add_popover_builder);
    g_clear_object (&self->settings_popover_builder);

    G_OBJECT_CLASS (otpclient_window_parent_class)->dispose (object);
}

static void
otpclient_window_class_init (OtpclientWindowClass *klass)
{
    G_OBJECT_CLASS (klass)->dispose = otpclient_window_dispose;
}

static void
otpclient_window_init (OtpclientWindow *self)
{
    (void) self;
}

OtpclientWindow *
otpclient_window_new (GtkApplication *app,
                      gint            width,
                      gint            height,
                      AppData        *app_data)
{
    /* Load all three UI files; the builder instantiates OtpclientWindow from the
     * XML template ("appwindow_id"), so we do NOT call g_object_new() separately. */
    GtkBuilder *builder              = get_builder_from_partial_path (UI_PARTIAL_PATH);
    GtkBuilder *add_popover_builder  = get_builder_from_partial_path (AP_PARTIAL_PATH);
    GtkBuilder *settings_popover_builder = get_builder_from_partial_path (SP_PARTIAL_PATH);

    GtkWidget *main_window = GTK_WIDGET (gtk_builder_get_object (builder, "appwindow_id"));
    if (main_window == NULL) {
        g_object_unref (builder);
        g_object_unref (add_popover_builder);
        g_object_unref (settings_popover_builder);
        return NULL;
    }

    /* Transfer builder ownership to the window instance */
    OtpclientWindow *win = OTPCLIENT_WINDOW (main_window);
    win->builder                   = builder;
    win->add_popover_builder       = add_popover_builder;
    win->settings_popover_builder  = settings_popover_builder;

    /* Publish non-owning references so the rest of the code can reach them */
    app_data->builder                   = builder;
    app_data->add_popover_builder       = add_popover_builder;
    app_data->settings_popover_builder  = settings_popover_builder;
    app_data->main_window               = main_window;

    gtk_window_set_application (GTK_WINDOW (main_window), app);
    gtk_window_set_icon_name (GTK_WINDOW (main_window), "otpclient");
    gtk_window_set_default_size (GTK_WINDOW (main_window),
                                 (width  >= 150) ? width  : 500,
                                 (height >= 150) ? height : 300);

    /* Lock button */
    GtkWidget *lock_btn = GTK_WIDGET (gtk_builder_get_object (builder, "lock_btn_id"));
    if (app_data->use_secret_service) {
        /* Secret service stores the key, so manual locking is not available */
        gtk_widget_set_sensitive (lock_btn, FALSE);
    } else {
        g_signal_connect (lock_btn, "clicked", G_CALLBACK (lock_app), app_data);
    }

#ifdef ENABLE_MINIMIZE_TO_TRAY
    if (app_data->use_tray) {
        init_tray_icon (app_data);
    }
#endif

    /* Empty-state "add" shortcut button */
    GtkWidget *empty_state_add_btn =
        GTK_WIDGET (gtk_builder_get_object (builder, "empty_state_add_btn_id"));
    if (empty_state_add_btn != NULL) {
        g_signal_connect (empty_state_add_btn, "clicked",
                          G_CALLBACK (manual_add_cb_shortcut), app_data);
    }

    /* ── Action groups ─────────────────────────────────────────────── */

    static GActionEntry import_menu_entries[] = {
        { .name = FREEOTPPLUS_PLAIN_ACTION_NAME, .activate = import_data_cb },
        { .name = AEGIS_PLAIN_ACTION_NAME,       .activate = import_data_cb },
        { .name = AEGIS_ENC_ACTION_NAME,         .activate = import_data_cb },
        { .name = AUTHPRO_PLAIN_ACTION_NAME,     .activate = import_data_cb },
        { .name = AUTHPRO_ENC_ACTION_NAME,       .activate = import_data_cb },
        { .name = TWOFAS_PLAIN_ACTION_NAME,      .activate = import_data_cb },
        { .name = TWOFAS_ENC_ACTION_NAME,        .activate = import_data_cb },
        { .name = GOOGLE_FILE_ACTION_NAME,       .activate = add_qr_from_file },
        { .name = GOOGLE_WEBCAM_ACTION_NAME,     .activate = webcam_add_cb },
    };

    static GActionEntry export_menu_entries[] = {
        { .name = FREEOTPPLUS_PLAIN_ACTION_NAME, .activate = export_data_cb },
        { .name = AEGIS_PLAIN_ACTION_NAME,       .activate = export_data_cb },
        { .name = AEGIS_ENC_ACTION_NAME,         .activate = export_data_cb },
        { .name = AUTHPRO_PLAIN_ACTION_NAME,     .activate = export_data_cb },
        { .name = AUTHPRO_ENC_ACTION_NAME,       .activate = export_data_cb },
        { .name = TWOFAS_PLAIN_ACTION_NAME,      .activate = export_data_cb },
        { .name = TWOFAS_ENC_ACTION_NAME,        .activate = export_data_cb },
    };

    static GActionEntry settings_menu_entries[] = {
        { .name = "create_newdb",  .activate = new_db_cb },
        { .name = "change_db",     .activate = change_db_cb },
        { .name = "change_pwd",    .activate = change_password_cb },
        { .name = "change_db_sec", .activate = change_db_sec_cb },
        { .name = "settings",      .activate = settings_dialog_cb },
        { .name = "shortcuts",     .activate = shortcuts_window_cb },
        { .name = "dbinfo",        .activate = dbinfo_cb },
        { .name = "about",         .activate = about_diag_cb },
    };

    static GActionEntry add_menu_entries[] = {
        { .name = "webcam",              .activate = webcam_add_cb },
        { .name = "import_qr_file",      .activate = add_qr_from_file },
        { .name = "import_qr_clipboard", .activate = add_qr_from_clipboard },
        { .name = "manual",              .activate = manual_add_cb },
    };

    GtkWidget *settings_popover =
        GTK_WIDGET (gtk_builder_get_object (settings_popover_builder, "settings_pop_id"));
    gtk_menu_button_set_popover (
        GTK_MENU_BUTTON (gtk_builder_get_object (builder, "settings_btn_id")),
        settings_popover);

    GActionGroup *settings_actions = (GActionGroup *) g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (settings_actions),
                                     settings_menu_entries,
                                     G_N_ELEMENTS (settings_menu_entries), app_data);
    gtk_widget_insert_action_group (settings_popover, "settings_menu", settings_actions);
    g_object_unref (settings_actions);

    GActionGroup *export_actions = (GActionGroup *) g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (export_actions),
                                     export_menu_entries,
                                     G_N_ELEMENTS (export_menu_entries), app_data);
    gtk_widget_insert_action_group (settings_popover, "export_menu", export_actions);
    g_object_unref (export_actions);

    GtkWidget *add_popover =
        GTK_WIDGET (gtk_builder_get_object (add_popover_builder, "add_pop_id"));
    gtk_menu_button_set_popover (
        GTK_MENU_BUTTON (gtk_builder_get_object (builder, "add_btn_main_id")),
        add_popover);

    GActionGroup *add_actions = (GActionGroup *) g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (add_actions),
                                     add_menu_entries,
                                     G_N_ELEMENTS (add_menu_entries), app_data);
    gtk_widget_insert_action_group (add_popover, "add_menu", add_actions);
    g_object_unref (add_actions);

    GActionGroup *import_actions = (GActionGroup *) g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (import_actions),
                                     import_menu_entries,
                                     G_N_ELEMENTS (import_menu_entries), app_data);
    gtk_widget_insert_action_group (add_popover, "import_menu", import_actions);
    g_object_unref (import_actions);

    gtk_popover_set_constrain_to (GTK_POPOVER (add_popover),      GTK_POPOVER_CONSTRAINT_NONE);
    gtk_popover_set_constrain_to (GTK_POPOVER (settings_popover), GTK_POPOVER_CONSTRAINT_NONE);

    /* ── Reorder toggle ────────────────────────────────────────────── */
    GtkToggleButton *reorder_toggle_btn =
        GTK_TOGGLE_BUTTON (gtk_builder_get_object (builder, "reorder_toggle_btn_id"));
    g_signal_connect (reorder_toggle_btn, "toggled", G_CALLBACK (reorder_rows_cb), app_data);

    /* ── Window-level key handling (GtkEventControllerKey, GTK ≥ 3.24) */
    GtkEventController *key_controller = gtk_event_controller_key_new (main_window);
    g_signal_connect (key_controller, "key-pressed", G_CALLBACK (key_pressed_cb), app_data);
    g_object_unref (key_controller);

    /* ── Track window size via configure-event (fires only on resize) */
    g_signal_connect (main_window, "configure-event",
                      G_CALLBACK (configure_event_cb), app_data);

    /* Hide search entry without triggering an extra layout pass on show_all */
    GtkWidget *search_entry =
        GTK_WIDGET (gtk_builder_get_object (builder, "search_entry_id"));
    if (search_entry != NULL) {
        gtk_widget_set_no_show_all (search_entry, TRUE);
        gtk_widget_set_visible (search_entry, FALSE);
    }

    return win;
}

/* ── Builder accessors ──────────────────────────────────────────────────── */

GtkBuilder *
otpclient_window_get_builder (OtpclientWindow *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_WINDOW (self), NULL);
    return self->builder;
}

GtkBuilder *
otpclient_window_get_add_popover_builder (OtpclientWindow *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_WINDOW (self), NULL);
    return self->add_popover_builder;
}

GtkBuilder *
otpclient_window_get_settings_popover_builder (OtpclientWindow *self)
{
    g_return_val_if_fail (OTPCLIENT_IS_WINDOW (self), NULL);
    return self->settings_popover_builder;
}

/* ── Signal callbacks ───────────────────────────────────────────────────── */

static gboolean
key_pressed_cb (GtkEventControllerKey *controller UNUSED,
                guint                  keyval,
                guint                  keycode    UNUSED,
                GdkModifierType        state,
                gpointer               user_data)
{
    CAST_USER_DATA (AppData, app_data, user_data);
    switch (keyval) {
        case GDK_KEY_q:
            if (state & GDK_CONTROL_MASK) {
                gtk_window_close (GTK_WINDOW (app_data->main_window));
            }
            break;
        case GDK_KEY_f:
            if (state & GDK_CONTROL_MASK) {
                GtkWidget *search_entry = app_data->search_entry;
                if (search_entry != NULL) {
                    gboolean is_visible = gtk_widget_get_visible (search_entry);
                    gtk_widget_set_visible (search_entry, !is_visible);
                    if (!is_visible) {
                        gtk_widget_grab_focus (search_entry);
                    } else {
                        gtk_entry_set_text (GTK_ENTRY (search_entry), "");
                        if (app_data->tree_view != NULL) {
                            gtk_widget_grab_focus (GTK_WIDGET (app_data->tree_view));
                        }
                    }
                }
                return TRUE;
            }
            break;
    }
    return FALSE;
}

static void
reorder_rows_cb (GtkToggleButton *btn,
                 gpointer         user_data)
{
    CAST_USER_DATA (AppData, app_data, user_data);
    gboolean is_btn_active = gtk_toggle_button_get_active (btn);
    gtk_tree_view_set_reorderable (GTK_TREE_VIEW (app_data->tree_view), is_btn_active);
    app_data->is_reorder_active = is_btn_active;
    gtk_widget_set_sensitive (
        GTK_WIDGET (gtk_builder_get_object (app_data->builder, "add_btn_main_id")),
        !is_btn_active);

    if (!is_btn_active) {
        reorder_db (app_data);
    }
}

static gboolean
configure_event_cb (GtkWidget         *window,
                    GdkEventConfigure *event UNUSED,
                    gpointer           user_data)
{
    CAST_USER_DATA (AppData, app_data, user_data);
    gtk_window_get_size (GTK_WINDOW (window),
                         &app_data->window_width, &app_data->window_height);
    return FALSE;
}

/* ── destroy_cb ─────────────────────────────────────────────────────────── */

void
destroy_cb (GtkWidget *window,
            gpointer   user_data)
{
    CAST_USER_DATA (AppData, app_data, user_data);

    OtpclientApplication *app = NULL;
    if (window != NULL) {
        app = OTPCLIENT_APPLICATION (gtk_window_get_application (GTK_WINDOW (window)));
    } else {
        app = OTPCLIENT_APPLICATION (g_application_get_default ());
    }
    otpclient_application_clear_app_data (app);

    if (app_data->tree_view != NULL) {
        save_sort_order (app_data->tree_view);
        save_column_widths (app_data->tree_view);
    }
    if (app_data->source_id != 0) {
        g_source_remove (app_data->source_id);
    }
    if (app_data->source_id_last_activity != 0) {
        g_source_remove (app_data->source_id_last_activity);
    }
    if (app_data->last_user_activity != NULL) {
        g_date_time_unref (app_data->last_user_activity);
    }
    if (app_data->connection != NULL) {
        for (gint i = 0; i < DBUS_SERVICES; i++) {
            g_dbus_connection_signal_unsubscribe (app_data->connection,
                                                  app_data->subscription_ids[i]);
        }
        g_dbus_connection_close (app_data->connection, NULL, NULL, NULL);
    }
    if (app_data->db_data != NULL) {
        gcry_free (app_data->db_data->key);
        g_free (app_data->db_data->db_path);
        g_slist_free_full (app_data->db_data->objects_hash, g_free);
        json_decref (app_data->db_data->in_memory_json_data);
        g_clear_pointer (&app_data->db_data->last_hotp, g_free);
        g_clear_pointer (&app_data->db_data->last_hotp_update, g_date_time_unref);
        g_free (app_data->db_data);
    }
    if (app_data->clipboard != NULL) {
        gtk_clipboard_clear (app_data->clipboard);
    }
    if (app != NULL) {
        g_application_withdraw_notification (G_APPLICATION (app), NOTIFICATION_ID);
    }
    if (app_data->notification != NULL) {
        g_object_unref (app_data->notification);
    }

    gint w = app_data->window_width;
    gint h = app_data->window_height;
    if (w > 0 && h > 0) {
        save_window_size (w, h);
    }

    /* Builders are owned by the OtpclientWindow instance and will be released
     * when the window itself is disposed — do NOT unref them here. */

    if (app_data->filter_model != NULL) {
        g_object_unref (app_data->filter_model);
    }
    if (app_data->list_store != NULL) {
        g_object_unref (app_data->list_store);
    }
    g_free (app_data);
    gcry_control (GCRYCTL_TERM_SECMEM);
}
