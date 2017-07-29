#include <gtk/gtk.h>

gboolean
label_update (gpointer data)
{
    GtkWidget *label = (GtkWidget *)data;
    gint sec_expired = 59 - g_date_time_get_second (g_date_time_new_now_local ());
    gchar *label_text = g_strdup_printf ("Token validity: %ds", (sec_expired < 30) ? sec_expired : sec_expired-30);
    gtk_label_set_label (GTK_LABEL (label), label_text);
    g_free (label_text);

    return TRUE;
}