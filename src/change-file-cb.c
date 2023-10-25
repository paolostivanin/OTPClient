#include <gtk/gtk.h>
#include "new-db-cb.h"
#include "change-db-cb.h"
#include "db-misc.h"
#include "message-dialogs.h"

gboolean
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
            change_db_cb (NULL, NULL, app_data);
            res = TRUE;
            break;
        case GTK_RESPONSE_OK:
            // create a new db.
            new_db_cb (NULL, NULL, app_data);
            res = TRUE;
            break;
        case GTK_RESPONSE_CANCEL:
        default:
            break;
    }
    gtk_widget_hide (diag_changefile);

    return res;
}
