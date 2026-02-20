#include <gtk/gtk.h>
#include "config-misc.h"

static void store_data (const gchar *param1_name,
                        gint         param1_value,
                        const gchar *param2_name,
                        gint         param2_value);

void
save_sort_order (GtkTreeView *tree_view)
{
    gint id;
    GtkSortType order;
    GtkTreeModel *model = gtk_tree_view_get_model (tree_view);
    if (model == NULL) {
        return;
    }
    if (GTK_IS_TREE_MODEL_FILTER (model)) {
        model = gtk_tree_model_filter_get_model (GTK_TREE_MODEL_FILTER (model));
    }
    if (!GTK_IS_TREE_SORTABLE (model)) {
        return;
    }
    gtk_tree_sortable_get_sort_column_id (GTK_TREE_SORTABLE (model), &id, &order);
    if (id >= 0) {
        store_data ("column_id", id, "sort_order", order);
    }
}

void
save_window_size (gint width,
                  gint height)
{
    store_data ("window_width", width, "window_height", height);
}

static void
store_data (const gchar *param1_name,
            gint         param1_value,
            const gchar *param2_name,
            gint         param2_value)
{
    GError *err = NULL;
    GKeyFile *kf = g_key_file_new ();
    gchar *cfg_file_path;
#ifndef IS_FLATPAK
    cfg_file_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
#else
    cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
#endif
    if (g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
        if (!g_key_file_load_from_file (kf, cfg_file_path, G_KEY_FILE_NONE, &err)) {
            g_printerr ("%s\n", err->message);
            g_clear_error (&err);
        } else {
            g_key_file_set_integer (kf, "config", param1_name, param1_value);
            g_key_file_set_integer (kf, "config", param2_name, param2_value);
            if (!g_key_file_save_to_file (kf, cfg_file_path, &err)) {
                g_printerr ("%s\n", err->message);
                g_clear_error (&err);
            }
        }
    }
    g_key_file_free (kf);
    g_free (cfg_file_path);
}
