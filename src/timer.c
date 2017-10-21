#include <gtk/gtk.h>
#include "liststore-misc.h"


gboolean
label_update (gpointer data)
{
    GtkWidget *label = (GtkWidget *)data;
    gint sec_expired = 59 - g_date_time_get_second (g_date_time_new_now_local ());
    gint token_validity = (sec_expired < 30) ? sec_expired : sec_expired-30;
    gchar *label_text = g_strdup_printf ("Token validity: %ds", token_validity);
    gtk_label_set_label (GTK_LABEL (label), label_text);
    if (token_validity == 29) {
        DatabaseData *db_data = g_object_get_data (G_OBJECT (label), "db_data");
        GtkListStore *list_store = g_object_get_data (G_OBJECT (label), "lstore");
        traverse_liststore (list_store, db_data);
    }
    g_free (label_text);

    return TRUE;
}