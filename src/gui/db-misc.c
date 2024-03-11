#include <jansson.h>
#include "db-misc.h"
#include "otpclient.h"
#include "../common/gquarks.h"
#include "gui-misc.h"

static void  json_free (gpointer       data);


void
regenerate_model (AppData *app_data)
{
    update_model (app_data);
    g_slist_free_full (app_data->db_data->data_to_add, json_free);
    app_data->db_data->data_to_add = NULL;
}


void
load_new_db (AppData  *app_data,
             GError  **err)
{
    reload_db (app_data->db_data, err);
    if (*err != NULL) {
        return;
    }

    update_model (app_data);
    g_slist_free_full (app_data->db_data->data_to_add, json_free);
    app_data->db_data->data_to_add = NULL;
}


void
write_db_to_disk (DatabaseData  *db_data,
                  GError       **err)
{
    update_db (db_data, err);
}


static void
json_free (gpointer data)
{
    json_decref (data);
}