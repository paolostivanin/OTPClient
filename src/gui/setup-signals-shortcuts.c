#include <gtk/gtk.h>
#include "data.h"
#include "otpclient-window.h"
#include "change-pwd-cb.h"
#include "settings-cb.h"
#include "shortcuts-cb.h"
#include "webcam-add-cb.h"
#include "manual-add-cb.h"
#include "edit-row-cb.h"
#include "show-qr-cb.h"
#include "change-db-cb.h"
#include "../common/macros.h"

static const char *signal_names[] = {
    "toggle-reorder-button", "lock-app",
    "change-db", "change-pwd", "show-settings", "show-kb-shortcuts",
    "scan-webcam", "manual-add", "edit-row", "show-qr"
};

static void setup_signals   (void);
static void connect_signals (AppData *app_data);

static void
toggle_reorder_cb_shortcut (GtkWidget *window UNUSED,
                             gpointer   user_data)
{
    CAST_USER_DATA (AppData, app_data, user_data);
    GtkToggleButton *btn =
        GTK_TOGGLE_BUTTON (gtk_builder_get_object (app_data->builder,
                                                    "reorder_toggle_btn_id"));
    if (btn != NULL) {
        gtk_toggle_button_set_active (btn, !gtk_toggle_button_get_active (btn));
    }
}


void
setup_kb_shortcuts (AppData *app_data)
{
    /* Used letters: r, l, h, w, m, b, o, e, s, k
     * hide-all-otps is registered in src/gui/treeview.c */
    setup_signals ();
    connect_signals (app_data);

    GtkBindingSet *mw_binding_set =
        gtk_binding_set_by_class (GTK_APPLICATION_WINDOW_GET_CLASS (app_data->main_window));

    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_r, GDK_CONTROL_MASK, signal_names[0], 0);
    if (app_data->auto_lock == TRUE || app_data->inactivity_timeout > 0) {
        /* auto-lock is enabled → secret service is disabled → allow Ctrl+L shortcut */
        gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_l, GDK_CONTROL_MASK, signal_names[1], 0);
    }
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_b, GDK_CONTROL_MASK, signal_names[2], 0);
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_o, GDK_CONTROL_MASK, signal_names[3], 0);
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_s, GDK_CONTROL_MASK, signal_names[4], 0);
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_k, GDK_CONTROL_MASK, signal_names[5], 0);

    /* GDK_MOD1_MASK: Alt key (X11) */
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_w, GDK_MOD1_MASK, signal_names[6], 0);
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_m, GDK_MOD1_MASK, signal_names[7], 0);
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_e, GDK_MOD1_MASK, signal_names[8], 0);
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_q, GDK_MOD1_MASK, signal_names[9], 0);
}


static void
setup_signals (void)
{
    for (int i = 0; i < (int) G_N_ELEMENTS (signal_names); ++i) {
        if (g_signal_lookup (signal_names[i], OTPCLIENT_TYPE_WINDOW) == 0) {
            g_signal_new (signal_names[i], OTPCLIENT_TYPE_WINDOW,
                          G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION,
                          0, NULL, NULL, NULL,
                          G_TYPE_NONE, 0);
        }
    }
}


static void
connect_signals (AppData *app_data)
{
    static const struct {
        const char *signal_name;
        GCallback   callback;
    } signal_connections[] = {
        { "toggle-reorder-button", G_CALLBACK (toggle_reorder_cb_shortcut) },
        { "change-db",             G_CALLBACK (change_db_cb_shortcut)       },
        { "change-pwd",            G_CALLBACK (change_pwd_cb_shortcut)      },
        { "show-settings",         G_CALLBACK (show_settings_cb_shortcut)   },
        { "show-kb-shortcuts",     G_CALLBACK (show_kbs_cb_shortcut)        },
        { "scan-webcam",           G_CALLBACK (webcam_add_cb_shortcut)      },
        { "manual-add",            G_CALLBACK (manual_add_cb_shortcut)      },
        { "edit-row",              G_CALLBACK (edit_row_cb_shortcut)        },
        { "show-qr",               G_CALLBACK (show_qr_cb_shortcut)        },
    };

    for (int i = 0; i < (int) G_N_ELEMENTS (signal_connections); ++i) {
        g_signal_connect (app_data->main_window,
                          signal_connections[i].signal_name,
                          signal_connections[i].callback,
                          app_data);
    }
}
