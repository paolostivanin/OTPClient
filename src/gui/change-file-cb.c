#include "gtk-compat.h"
#include "new-db-cb.h"
#include "change-db-cb.h"
#include "change-file-cb.h"
#include "message-dialogs.h"

int
change_file (AppData *app_data)
{
    GtkWidget *label = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "diag_changefile_label_id"));
    gchar *partial_msg_start = g_markup_printf_escaped ("%s <span font_family=\"monospace\">%s</span>", "The currently selected file is:\n", app_data->db_data->db_path);
    const gchar *partial_msg_end = "\n\nWhat would you like to do?";
    gchar *msg = g_strconcat (partial_msg_start, partial_msg_end, NULL);
    gtk_label_set_markup (GTK_LABEL(label), msg);
    g_free (msg);
    g_free (partial_msg_start);

    gboolean res = FALSE;
    GtkWidget *diag_changefile = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "diag_changefile_id"));
    gint result = gtk_dialog_run (GTK_DIALOG(diag_changefile));
    switch (result) {
        case GTK_RESPONSE_ACCEPT:
            // select an existing DB.
            res = change_db (app_data);
            break;
        case GTK_RESPONSE_OK:
            // create a new db.
            res =  new_db (app_data);
            break;
        case GTK_RESPONSE_CLOSE:
            res = QUIT_APP;
        default:
            break;
    }

    gtk_widget_set_visible (diag_changefile, FALSE);

    return res;
}
