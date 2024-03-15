#include <glib.h>
#include "import-export.h"
#include "get-providers-data.h"

GSList *
get_data_from_provider (const gchar  *action_name,
                        const gchar  *filename,
                        const gchar  *pwd,
                        gint32        max_file_size_from_memlock,
                        GError      **err)
{
    GSList *content = NULL;
    if (g_strcmp0 (action_name, ANDOTP_PLAIN_ACTION_NAME) == 0 || g_strcmp0 (action_name, ANDOTP_ENC_ACTION_NAME) == 0) {
        content = get_andotp_data (filename, pwd, max_file_size_from_memlock, err);
    } else if (g_strcmp0 (action_name, FREEOTPPLUS_PLAIN_ACTION_NAME) == 0) {
        content = get_freeotpplus_data (filename, max_file_size_from_memlock, err);
    } else if (g_strcmp0 (action_name, AEGIS_PLAIN_ACTION_NAME) == 0 || g_strcmp0 (action_name, AEGIS_ENC_ACTION_NAME) == 0) {
        content = get_aegis_data (filename, pwd, max_file_size_from_memlock, err);
    } else if (g_strcmp0 (action_name, AUTHPRO_PLAIN_ACTION_NAME) == 0 || g_strcmp0 (action_name, AUTHPRO_ENC_ACTION_NAME) == 0) {
        content = get_authpro_data (filename, pwd, max_file_size_from_memlock, err);
    } else if (g_strcmp0 (action_name, TWOFAS_PLAIN_ACTION_NAME) == 0 || g_strcmp0 (action_name, TWOFAS_ENC_ACTION_NAME) == 0) {
        content = get_twofas_data (filename, pwd, max_file_size_from_memlock, err);
    }

    return content;
}