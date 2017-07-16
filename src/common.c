#include <gtk/gtk.h>

static void icon_press_cb (GtkEntry *entry, gint position, GdkEventButton *event, gpointer data);


void
set_icon_to_entry (GtkWidget *entry, const gchar *icon_name, const gchar *tooltip_text)
{
    GIcon *gicon = g_themed_icon_new_with_default_fallbacks (icon_name);
    gtk_entry_set_icon_from_gicon (GTK_ENTRY (entry), GTK_ENTRY_ICON_SECONDARY, gicon);
    gtk_entry_set_icon_activatable (GTK_ENTRY (entry), GTK_ENTRY_ICON_SECONDARY, TRUE);
    gtk_entry_set_icon_tooltip_text (GTK_ENTRY (entry), GTK_ENTRY_ICON_SECONDARY, tooltip_text);
    gtk_entry_set_visibility (GTK_ENTRY (entry), FALSE);
    g_signal_connect (entry, "icon-press", G_CALLBACK(icon_press_cb), NULL);
}


static void
icon_press_cb (GtkEntry *entry,
               gint position __attribute__((__unused__)),
               GdkEventButton *event __attribute__((__unused__)),
               gpointer data __attribute__((__unused__)))
{
    gtk_entry_set_visibility (GTK_ENTRY (entry), !gtk_entry_get_visibility (entry));
}