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

    gtk_binding_entry_add_signal(mw_binding_set, GDK_KEY_r, GDK_CONTROL_MASK, "toggle-reorder-button", 0);
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_d, GDK_CONTROL_MASK, "toggle-delete-button", 0);
    if (app_data->auto_lock == TRUE || app_data->inactivity_timeout > 0) {
        // auto-lock is enabled, so secret service is disabled, therefore we allow the shortcut
        gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_l, GDK_CONTROL_MASK, "lock-app", 0);
    }
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_b, GDK_CONTROL_MASK, "change-db", 0);
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_o, GDK_CONTROL_MASK, "change-pwd", 0);
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_s, GDK_CONTROL_MASK, "show-settings", 0);
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_k, GDK_CONTROL_MASK, "show-kb-shortcuts", 0);

    // GDM_MOD1_MASK: the fourth modifier key (it depends on the modifier mapping of the X server which key is interpreted as this modifier, but normally it is the Alt key).
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_w, GDK_MOD1_MASK, "scan-webcam", 0);
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_m, GDK_MOD1_MASK, "manual-add", 0);
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_e, GDK_MOD1_MASK, "edit-row", 0);
    gtk_binding_entry_add_signal (mw_binding_set, GDK_KEY_q, GDK_MOD1_MASK, "show-qr", 0);
}


static void
setup_signals (void)
{
    g_signal_new ("toggle-reorder-button", G_TYPE_OBJECT, G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    g_signal_new ("toggle-delete-button", G_TYPE_OBJECT, G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    g_signal_new ("lock-app", G_TYPE_OBJECT, G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    g_signal_new ("change-db", G_TYPE_OBJECT, G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    g_signal_new ("change-pwd", G_TYPE_OBJECT, G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    g_signal_new ("show-settings", G_TYPE_OBJECT, G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    g_signal_new ("show-kb-shortcuts", G_TYPE_OBJECT, G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    g_signal_new ("scan-webcam", G_TYPE_OBJECT, G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    g_signal_new ("manual-add", G_TYPE_OBJECT, G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    g_signal_new ("edit-row", G_TYPE_OBJECT, G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
    g_signal_new ("show-qr", G_TYPE_OBJECT, G_SIGNAL_RUN_FIRST | G_SIGNAL_ACTION, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}


static void
connect_signals (AppData *app_data)
{
    g_signal_connect (app_data->main_window, "change-db", G_CALLBACK(change_db_cb_shortcut), app_data);
    g_signal_connect (app_data->main_window, "change-pwd", G_CALLBACK(change_pwd_cb_shortcut), app_data);
    g_signal_connect (app_data->main_window, "show-settings", G_CALLBACK(show_settings_cb_shortcut), app_data);
    g_signal_connect (app_data->main_window, "show-kb-shortcuts", G_CALLBACK(show_kbs_cb_shortcut), app_data);
    g_signal_connect (app_data->main_window, "scan-webcam", G_CALLBACK(webcam_add_cb_shortcut), app_data);
    g_signal_connect (app_data->main_window, "manual-add", G_CALLBACK(manual_add_cb_shortcut), app_data);
    g_signal_connect (app_data->main_window, "edit-row", G_CALLBACK(edit_row_cb_shortcut), app_data);
    g_signal_connect (app_data->main_window, "show-qr", G_CALLBACK(show_qr_cb_shortcut), app_data);
}