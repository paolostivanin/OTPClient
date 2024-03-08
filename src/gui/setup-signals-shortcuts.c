#include <gtk/gtk.h>
#include "data.h"
#include "change-pwd-cb.h"
#include "settings-cb.h"
#include "shortcuts-cb.h"
#include "webcam-add-cb.h"
#include "manual-add-cb.h"
#include "edit-row-cb.h"
#include "show-qr-cb.h"
#include "change-db-cb.h"

static const char *signal_names[] = {
        "toggle-reorder-button", "toggle-delete-button", "lock-app",
        "change-db", "change-pwd", "show-settings", "show-kb-shortcuts",
        "scan-webcam", "manual-add", "edit-row", "show-qr"
};

static void setup_signals   (void);

static void connect_signals (AppData *app_data);


void
setup_kb_shortcuts (AppData *app_data)
{
    // Used letters: r,d,l,h,w,m,b,o,e,s,k
    // hide-all-otps is in src/treeview.c
    setup_signals ();
    connect_signals (app_data);

    GtkBindingSet *mw_binding_set = gtk_binding_set_by_class (GTK_APPLICATION_WINDOW_GET_CLASS(app_data->main_window));

    gtk_binding_entry_add_signal(mw_binding_set, GDK_KEY_r, GDK_CONTROL_MASK, signal_names[0], 0);
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_d, GDK_CONTROL_MASK, signal_names[1], 0);
    if (app_data->auto_lock == TRUE || app_data->inactivity_timeout > 0) {
        // auto-lock is enabled, so secret service is disabled, therefore we allow the shortcut
        gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_l, GDK_CONTROL_MASK, signal_names[2], 0);
    }
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_b, GDK_CONTROL_MASK, signal_names[3], 0);
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_o, GDK_CONTROL_MASK, signal_names[4], 0);
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_s, GDK_CONTROL_MASK, signal_names[5], 0);
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_k, GDK_CONTROL_MASK, signal_names[6], 0);

    // GDM_MOD1_MASK: the fourth modifier key (it depends on the modifier mapping of the X server which key is interpreted as this modifier, but normally it is the Alt key).
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_w, GDK_MOD1_MASK, signal_names[7], 0);
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_m, GDK_MOD1_MASK, signal_names[8], 0);
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_e, GDK_MOD1_MASK, signal_names[9], 0);
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_q, GDK_MOD1_MASK, signal_names[10], 0);
}


static void
setup_signals (void)
{
    for (int i = 0; i < G_N_ELEMENTS(signal_names); ++i) {
        g_signal_new (signal_names[i], G_TYPE_OBJECT, G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    }
}


static void
connect_signals (AppData *app_data)
{
    struct {
        const char *signal_name;
        GCallback callback;
    } signal_connections[] = {
            {"change-db", G_CALLBACK(change_db_cb_shortcut)},
            {"change-pwd", G_CALLBACK(change_pwd_cb_shortcut)},
            {"show-settings", G_CALLBACK(show_settings_cb_shortcut)},
            {"show-kb-shortcuts", G_CALLBACK(show_kbs_cb_shortcut)},
            {"scan-webcam", G_CALLBACK(webcam_add_cb_shortcut)},
            {"manual-add", G_CALLBACK(manual_add_cb_shortcut)},
            {"edit-row", G_CALLBACK(edit_row_cb_shortcut)},
            {"show-qr", G_CALLBACK(show_qr_cb_shortcut)}
    };

    for (int i = 0; i < G_N_ELEMENTS(signal_connections); ++i) {
        g_signal_connect (app_data->main_window, signal_connections[i].signal_name, signal_connections[i].callback, app_data);
    }
}