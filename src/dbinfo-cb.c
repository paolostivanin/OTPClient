#include "data.h"


void
dbinfo_cb (GSimpleAction *simple    __attribute__((unused)),
              GVariant      *parameter __attribute__((unused)),
              gpointer       user_data)
{
    AppData *app_data = (AppData *)user_data;

    GtkWidget *dbinfo_diag = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "dbinfo_diag_id"));
    GtkWidget *db_location_entry = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "dbentry_dbinfo_id"));
    GtkWidget *config_entry = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "configentry_dbinfo_id"));
    GtkWidget *num_entries_label = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "numofentries_dbinfo_id"));

    gtk_entry_set_text (GTK_ENTRY(db_location_entry), app_data->db_data->db_path);
    gchar *cfg_file_path = NULL;
#ifdef USE_FLATPAK_APP_FOLDER
    cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#else
    cfg_file_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
#endif
    gtk_entry_set_text (GTK_ENTRY(config_entry), cfg_file_path);

    gchar *num_of_entries = g_strdup_printf ("%lu", json_array_size (app_data->db_data->json_data));
    gtk_label_set_text (GTK_LABEL(num_entries_label), num_of_entries);

    gint result = gtk_dialog_run (GTK_DIALOG(dbinfo_diag));
    if (result == GTK_RESPONSE_CLOSE) {
        g_free (cfg_file_path);
        g_free (num_of_entries);
        gtk_widget_hide (dbinfo_diag);
    }
}