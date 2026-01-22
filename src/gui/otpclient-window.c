#include <gtk/gtk.h>
#include <gcrypt.h>
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
#ifdef ENABLE_MINIMIZE_TO_TRAY
#include "tray.h"
#endif

static void       get_window_size_cb (GtkWidget         *window,
                                      GtkAllocation     *allocation,
                                      gpointer           user_data);

static gboolean   key_pressed_cb     (GtkWidget         *window,
                                      GdkEventKey       *event_key,
                                      gpointer           user_data);

static void       toggle_button_cb   (GtkWidget         *main_window,
                                      gpointer           user_data);

static void       reorder_rows_cb    (GtkToggleButton   *btn,
                                      gpointer           user_data);

static void       save_sort_order    (GtkTreeView       *tree_view);

static void       save_window_size   (gint               width,
                                      gint               height);

static void       store_data         (const gchar       *param1_name,
                                      gint               param1_value,
                                      const gchar       *param2_name,
                                      gint               param2_value);

struct _OtpclientWindow {
    GtkApplicationWindow parent_instance;
};

struct _OtpclientWindowClass {
    GtkApplicationWindowClass parent_class;
};

G_DEFINE_TYPE (OtpclientWindow, otpclient_window, GTK_TYPE_APPLICATION_WINDOW)

static void
otpclient_window_class_init (OtpclientWindowClass *klass)
{
    (void) klass;
    g_signal_new ("toggle-reorder-button", OTPCLIENT_TYPE_WINDOW,
                  G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION, 0, NULL, NULL, NULL,
                  G_TYPE_NONE, 0);
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
    app_data->main_window = GTK_WIDGET (gtk_builder_get_object (app_data->builder, "appwindow_id"));
    if (app_data->main_window == NULL) {
        return NULL;
    }

    gtk_window_set_application (GTK_WINDOW (app_data->main_window), app);
    gtk_window_set_icon_name (GTK_WINDOW (app_data->main_window), "otpclient");
    gtk_window_set_default_size (GTK_WINDOW (app_data->main_window), (width >= 150) ? width : 500, (height >= 150) ? height : 300);

    GtkWidget *lock_btn = GTK_WIDGET (gtk_builder_get_object (app_data->builder, "lock_btn_id"));
    g_signal_connect (lock_btn, "clicked", G_CALLBACK (lock_app), app_data);
#ifdef ENABLE_MINIMIZE_TO_TRAY
    if (app_data->use_tray) {
        init_tray_icon (app_data);
    }
#endif
    if (app_data->use_secret_service == TRUE) {
        // secret service is enabled, so we can't lock the app
        gtk_widget_set_sensitive (lock_btn, FALSE);
    }

    static GActionEntry import_menu_entries[] = {
        { .name = FREEOTPPLUS_PLAIN_ACTION_NAME, .activate = import_data_cb },
        { .name = AEGIS_PLAIN_ACTION_NAME, .activate = import_data_cb },
        { .name = AEGIS_ENC_ACTION_NAME, .activate = import_data_cb },
        { .name = AUTHPRO_PLAIN_ACTION_NAME, .activate = import_data_cb },
        { .name = AUTHPRO_ENC_ACTION_NAME, .activate = import_data_cb },
        { .name = TWOFAS_PLAIN_ACTION_NAME, .activate = import_data_cb },
        { .name = TWOFAS_ENC_ACTION_NAME, .activate = import_data_cb },
        { .name = GOOGLE_FILE_ACTION_NAME, .activate = add_qr_from_file },
        { .name = GOOGLE_WEBCAM_ACTION_NAME, .activate = webcam_add_cb }
    };

    static GActionEntry export_menu_entries[] = {
        { .name = FREEOTPPLUS_PLAIN_ACTION_NAME, .activate = export_data_cb },
        { .name = AEGIS_PLAIN_ACTION_NAME, .activate = export_data_cb },
        { .name = AEGIS_ENC_ACTION_NAME, .activate = export_data_cb },
        { .name = AUTHPRO_PLAIN_ACTION_NAME, .activate = export_data_cb },
        { .name = AUTHPRO_ENC_ACTION_NAME, .activate = export_data_cb },
        { .name = TWOFAS_PLAIN_ACTION_NAME, .activate = export_data_cb },
        { .name = TWOFAS_ENC_ACTION_NAME, .activate = export_data_cb }
    };

    static GActionEntry settings_menu_entries[] = {
        { .name = "create_newdb", .activate = new_db_cb },
        { .name = "change_db", .activate = change_db_cb },
        { .name = "change_pwd", .activate = change_password_cb },
        { .name = "change_db_sec", .activate = change_db_sec_cb },
        { .name = "settings", .activate = settings_dialog_cb },
        { .name = "shortcuts", .activate = shortcuts_window_cb },
        { .name = "dbinfo", .activate = dbinfo_cb },
        { .name = "about", .activate = about_diag_cb }
    };

    static GActionEntry add_menu_entries[] = {
        { .name = "webcam", .activate = webcam_add_cb },
        { .name = "import_qr_file", .activate = add_qr_from_file },
        { .name = "import_qr_clipboard", .activate = add_qr_from_clipboard },
        { .name = "manual", .activate = manual_add_cb }
    };

    GtkWidget *settings_popover = GTK_WIDGET (gtk_builder_get_object (app_data->settings_popover_builder, "settings_pop_id"));
    gtk_menu_button_set_popover (GTK_MENU_BUTTON (gtk_builder_get_object (app_data->builder, "settings_btn_id")), settings_popover);

    GActionGroup *settings_actions = (GActionGroup *) g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (settings_actions), settings_menu_entries, G_N_ELEMENTS (settings_menu_entries), app_data);
    gtk_widget_insert_action_group (settings_popover, "settings_menu", settings_actions);

    GActionGroup *export_actions = (GActionGroup *) g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (export_actions), export_menu_entries, G_N_ELEMENTS (export_menu_entries), app_data);
    gtk_widget_insert_action_group (settings_popover, "export_menu", export_actions);

    GtkWidget *add_popover = GTK_WIDGET (gtk_builder_get_object (app_data->add_popover_builder, "add_pop_id"));
    gtk_menu_button_set_popover (GTK_MENU_BUTTON (gtk_builder_get_object (app_data->builder, "add_btn_main_id")), add_popover);

    GActionGroup *add_actions = (GActionGroup *) g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (add_actions), add_menu_entries, G_N_ELEMENTS (add_menu_entries), app_data);
    gtk_widget_insert_action_group (add_popover, "add_menu", add_actions);

    GActionGroup *import_actions = (GActionGroup *) g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (import_actions), import_menu_entries, G_N_ELEMENTS (import_menu_entries), app_data);
    gtk_widget_insert_action_group (add_popover, "import_menu", import_actions);

    gtk_popover_set_constrain_to (GTK_POPOVER (add_popover), GTK_POPOVER_CONSTRAINT_NONE);
    gtk_popover_set_constrain_to (GTK_POPOVER (settings_popover), GTK_POPOVER_CONSTRAINT_NONE);

    GtkToggleButton *reorder_toggle_btn = GTK_TOGGLE_BUTTON (gtk_builder_get_object (app_data->builder, "reorder_toggle_btn_id"));
    g_signal_connect (app_data->main_window, "toggle-reorder-button", G_CALLBACK (toggle_button_cb), reorder_toggle_btn);
    g_signal_connect (reorder_toggle_btn, "toggled", G_CALLBACK (reorder_rows_cb), app_data);
    g_signal_connect (app_data->main_window, "key_press_event", G_CALLBACK (key_pressed_cb), app_data);
    g_signal_connect (app_data->main_window, "destroy", G_CALLBACK (destroy_cb), app_data);
    g_signal_connect (app_data->main_window, "size-allocate", G_CALLBACK (get_window_size_cb), NULL);

    return OTPCLIENT_WINDOW (app_data->main_window);
}

static gboolean
key_pressed_cb (GtkWidget   *window,
                GdkEventKey *event_key,
                gpointer     user_data)
{
    CAST_USER_DATA (AppData, app_data, user_data);
    switch (event_key->keyval) {
        case GDK_KEY_q:
            if (event_key->state & GDK_CONTROL_MASK) {
                gtk_window_close (GTK_WINDOW (window));
            }
            break;
        case GDK_KEY_f:
            if (event_key->state & GDK_CONTROL_MASK) {
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
toggle_button_cb (GtkWidget *main_window UNUSED,
                  gpointer   user_data)
{
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (user_data), !gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (user_data)));
}

static void
reorder_rows_cb (GtkToggleButton *btn,
                 gpointer         user_data)
{
    CAST_USER_DATA (AppData, app_data, user_data);
    gboolean is_btn_active = gtk_toggle_button_get_active (btn);
    gtk_tree_view_set_reorderable (GTK_TREE_VIEW (app_data->tree_view), is_btn_active);
    app_data->is_reorder_active = is_btn_active;
    gtk_widget_set_sensitive (GTK_WIDGET (gtk_builder_get_object (app_data->builder, "add_btn_main_id")), !is_btn_active);

    if (is_btn_active == FALSE) {
        // reordering has been disabled, so now we have to reorder and update the database itself
        reorder_db (app_data);
    }
}

static void
get_window_size_cb (GtkWidget     *window,
                    GtkAllocation *allocation UNUSED,
                    gpointer       user_data UNUSED)
{
    gint w, h;
    gtk_window_get_size (GTK_WINDOW (window), &w, &h);
    g_object_set_data (G_OBJECT (window), "width", GINT_TO_POINTER (w));
    g_object_set_data (G_OBJECT (window), "height", GINT_TO_POINTER (h));
}

void
destroy_cb (GtkWidget *window,
            gpointer   user_data)
{
    CAST_USER_DATA (AppData, app_data, user_data);

    OtpclientApplication *app = OTPCLIENT_APPLICATION (gtk_window_get_application (GTK_WINDOW (window)));
    otpclient_application_clear_app_data (app);

    save_sort_order (app_data->tree_view);
    g_source_remove (app_data->source_id);
    g_source_remove (app_data->source_id_last_activity);
    g_date_time_unref (app_data->last_user_activity);
    for (gint i = 0; i < DBUS_SERVICES; i++) {
        g_dbus_connection_signal_unsubscribe (app_data->connection, app_data->subscription_ids[i]);
    }
    g_dbus_connection_close (app_data->connection, NULL, NULL, NULL);
    gcry_free (app_data->db_data->key);
    g_free (app_data->db_data->db_path);
    g_slist_free_full (app_data->db_data->objects_hash, g_free);
    json_decref (app_data->db_data->in_memory_json_data);
    g_free (app_data->db_data);
    gtk_clipboard_clear (app_data->clipboard);
    g_application_withdraw_notification (G_APPLICATION (gtk_window_get_application (GTK_WINDOW (app_data->main_window))), NOTIFICATION_ID);
    g_object_unref (app_data->notification);
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wbad-function-cast"
    gint w = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (window), "width"));
    gint h = GPOINTER_TO_INT (g_object_get_data (G_OBJECT (window), "height"));
#pragma GCC diagnostic pop
    save_window_size (w, h);
    g_object_unref (app_data->builder);
    g_object_unref (app_data->add_popover_builder);
    g_object_unref (app_data->settings_popover_builder);
    if (app_data->filter_model != NULL) {
        g_object_unref (app_data->filter_model);
    }
    if (app_data->list_store != NULL) {
        g_object_unref (app_data->list_store);
    }
    g_free (app_data);
    gcry_control (GCRYCTL_TERM_SECMEM);
}

static void
save_sort_order (GtkTreeView *tree_view)
{
    gint id;
    GtkSortType order;
    GtkTreeModel *model = gtk_tree_view_get_model (tree_view);
    if (GTK_IS_TREE_MODEL_FILTER (model)) {
        model = gtk_tree_model_filter_get_model (GTK_TREE_MODEL_FILTER (model));
    }
    gtk_tree_sortable_get_sort_column_id (GTK_TREE_SORTABLE (model), &id, &order);
    // store data only if it was changed
    if (id >= 0) {
        store_data ("column_id", id, "sort_order", order);
    }
}

static void
save_window_size (gint width,
                  gint height)
{
    store_data ("window_width", width, "window_height", height);
}

static void
store_data (const gchar *param1_name,
            gint         param1_value,
            const gchar *param2_name,
            gint         param2_value)
{
    GError *err = NULL;
    GKeyFile *kf = g_key_file_new ();
    gchar *cfg_file_path;
#ifndef IS_FLATPAK
    cfg_file_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
#else
    cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#endif
    if (g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
        if (!g_key_file_load_from_file (kf, cfg_file_path, G_KEY_FILE_NONE, &err)) {
            g_printerr ("%s\n", err->message);
            g_clear_error (&err);
        } else {
            g_key_file_set_integer (kf, "config", param1_name, param1_value);
            g_key_file_set_integer (kf, "config", param2_name, param2_value);
            if (!g_key_file_save_to_file (kf, cfg_file_path, &err)) {
                g_printerr ("%s\n", err->message);
                g_clear_error (&err);
            }
        }
    }
    g_key_file_free (kf);
    g_free (cfg_file_path);
}
