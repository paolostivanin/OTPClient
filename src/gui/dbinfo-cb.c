#include "data.h"
#include "../common/macros.h"


void
dbinfo_cb (GSimpleAction *simple UNUSED,
           GVariant      *parameter UNUSED,
           gpointer       user_data)
{
    CAST_USER_DATA(AppData, app_data, user_data);

    GtkWidget *dbinfo_diag = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "dbinfo_diag_id"));
    GtkWidget *db_location_entry = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "dbentry_dbinfo_id"));
    GtkWidget *config_entry = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "configentry_dbinfo_id"));
    GtkWidget *num_entries_label = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "numofentries_dbinfo_id"));
    GtkWidget *kdf_algo_label = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "kdfalgo_dbinfo_id"));
    GtkWidget *pbkdf2_label = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "pbkdf2_dbinfo_id"));
    GtkWidget *argon2id_label = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "argon2id_dbinfo_id"));
    GtkWidget *pbkdf2_params_label = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "pbkdf2_values_dbinfo_id"));
    GtkWidget *argon2id_params_label = GTK_WIDGET(gtk_builder_get_object (app_data->builder, "argon2id_values_dbinfo_id"));

    gchar *params = NULL;
    if (app_data->db_data->current_db_version >= 2) {
        gtk_widget_set_visible (pbkdf2_label, FALSE);
        gtk_widget_set_visible (pbkdf2_params_label, FALSE);
        gtk_label_set_label (GTK_LABEL(kdf_algo_label), "Argon2id");
        params = g_strconcat ("iters ", g_strdup_printf ("%d", ARGON2ID_ITER),
                              ", memcost ", g_strdup_printf ("%d", ARGON2ID_MEMCOST), " KiB"
                              ", parall. ", g_strdup_printf ("%d", ARGON2ID_PARALLELISM),
                              ", taglen ", g_strdup_printf ("%d", ARGON2ID_TAGLEN), " B", NULL);
        gtk_label_set_text(GTK_LABEL(argon2id_params_label), params);
        gtk_widget_set_visible (argon2id_label, TRUE);
        gtk_widget_set_visible (argon2id_params_label, TRUE);
    } else {
        gtk_widget_set_visible (argon2id_label, FALSE);
        gtk_widget_set_visible (argon2id_params_label, FALSE);
        gtk_label_set_label(GTK_LABEL(kdf_algo_label), "PBKDF2");
        params = g_strconcat ("SHA512 + ", g_strdup_printf ("%d", KDF_ITERATIONS), " iters", NULL);
        gtk_label_set_text(GTK_LABEL(pbkdf2_params_label), params);
        gtk_widget_set_visible (pbkdf2_label, TRUE);
        gtk_widget_set_visible (pbkdf2_params_label, TRUE);
    }
    g_free (params);

    gtk_entry_set_text (GTK_ENTRY(db_location_entry), app_data->db_data->db_path);
    gchar *cfg_file_path = NULL;
#ifdef IS_FLATPAK
    cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#else
    cfg_file_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
#endif
    gtk_entry_set_text (GTK_ENTRY(config_entry), cfg_file_path);

    gchar *num_of_entries = g_strdup_printf ("%lu", json_array_size (app_data->db_data->in_memory_json_data));
    gtk_label_set_text (GTK_LABEL(num_entries_label), num_of_entries);

    gint result = gtk_dialog_run (GTK_DIALOG(dbinfo_diag));
    if (result == GTK_RESPONSE_CLOSE) {
        g_free (cfg_file_path);
        g_free (num_of_entries);
        gtk_widget_hide (dbinfo_diag);
    }
}
