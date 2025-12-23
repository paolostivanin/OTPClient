#include "gtk-compat.h"
#include "data.h"
#include "../common/macros.h"
#include "get-builder.h"
#include "message-dialogs.h"
#include "../common/gquarks.h"

static gboolean validate_values (AppData      *app_data,
                                 const gchar  *new_iter,
                                 const gchar  *new_memcost,
                                 const gchar  *new_paral,
                                 GError      **err);


void
change_db_sec_cb (GSimpleAction *action_name UNUSED,
                  GVariant      *parameter UNUSED,
                  gpointer       user_data)
{
    CAST_USER_DATA(AppData, app_data, user_data);

    GtkBuilder *secset_builder = get_builder_from_partial_path (SECSET_PARTIAL_PATH);
    GtkWidget *secsettings_diag = GTK_WIDGET(gtk_builder_get_object (secset_builder, "change_db_sec_diag_id"));

    GtkWidget *secsettings_cur_iter_entry = GTK_WIDGET(gtk_builder_get_object (secset_builder, "change_db_sec_cur_iter_id"));
    GtkWidget *secsettings_cur_mc_entry = GTK_WIDGET(gtk_builder_get_object (secset_builder, "change_db_sec_cur_memcost_id"));
    GtkWidget *secsettings_cur_p_entry = GTK_WIDGET(gtk_builder_get_object (secset_builder, "change_db_sec_cur_paral_id"));
    gtk_editable_set_text (GTK_EDITABLE(secsettings_cur_iter_entry), g_strdup_printf ("%d", app_data->db_data->argon2id_iter));
    gtk_editable_set_text (GTK_EDITABLE(secsettings_cur_mc_entry), g_strdup_printf ("%d", app_data->db_data->argon2id_memcost));
    gtk_editable_set_text (GTK_EDITABLE(secsettings_cur_p_entry), g_strdup_printf ("%d", app_data->db_data->argon2id_parallelism));

    GtkWidget *secsettings_new_iter_entry = GTK_WIDGET(gtk_builder_get_object (secset_builder, "change_db_sec_new_iter_id"));
    GtkWidget *secsettings_new_mc_entry = GTK_WIDGET(gtk_builder_get_object (secset_builder, "change_db_sec_new_memcost_id"));
    GtkWidget *secsettings_new_p_entry = GTK_WIDGET(gtk_builder_get_object (secset_builder, "change_db_sec_new_paral_id"));

    gboolean res = FALSE;
    GError *err = NULL;
    gint result = gtk_dialog_run (GTK_DIALOG (secsettings_diag));
    switch (result) {
        case GTK_RESPONSE_OK:
            res = validate_values (app_data,
                                   gtk_editable_get_text (GTK_EDITABLE(secsettings_new_iter_entry)),
                                   gtk_editable_get_text (GTK_EDITABLE(secsettings_new_mc_entry)),
                                   gtk_editable_get_text (GTK_EDITABLE(secsettings_new_p_entry)),
                                   &err);
            if (res) {
                update_db (app_data->db_data, &err);
                if (err != NULL) {
                    gchar *msg = g_strconcat ("Couldn't update the database: ", err->message, NULL);
                    show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
                    g_free (msg);
                } else {
                    reload_db (app_data->db_data, &err);
                    if (err != NULL) {
                        gchar *msg = g_strconcat ("Couldn't reload the database: ", err->message, NULL);
                        show_message_dialog (app_data->main_window, msg, GTK_MESSAGE_ERROR);
                        g_free (msg);
                    }
                }
            } else {
                show_message_dialog (app_data->main_window, err->message, GTK_MESSAGE_ERROR);
            }
            break;
        case GTK_RESPONSE_CANCEL:
        default:
            break;
    }
    gtk_window_destroy (GTK_WINDOW(secsettings_diag));
    g_object_unref (secset_builder);
}


static gboolean
validate_values (AppData      *app_data,
                 const gchar  *new_iter,
                 const gchar  *new_memcost,
                 const gchar  *new_paral,
                 GError      **err)
{
    g_return_val_if_fail (err == NULL || *err == NULL, FALSE);

    // validate settings (establish a minimum and a maximum)
    for (gint i = 0; i < g_utf8_strlen (new_iter, -1); i++) {
        if (!g_ascii_isdigit(new_iter[i])) {
            g_set_error (err, validation_error_gquark (), NONDIGITS_ERRCODE, "The provided iterations value contains non digits characters.");
            return FALSE;
        }
    }
    gint32 iter = (gint32)g_ascii_strtoll (new_iter, NULL, 10);
    if (iter < 2 || iter > 8) {
        g_set_error (err, validation_error_gquark (), OUTOFRANGE_ERRCODE, "Iterations must be between 2 and 8.");
        return FALSE;
    }

    for (gint i = 0; i < g_utf8_strlen (new_memcost, -1); i++) {
        if (!g_ascii_isdigit(new_memcost[i])) {
            g_set_error (err, validation_error_gquark (), NONDIGITS_ERRCODE, "The provided memory cost value contains non digits characters.");
            return FALSE;
        }
    }
    gint32 memcost = (gint32)g_ascii_strtoll (new_memcost, NULL, 10);
    if (memcost < 16*1024 || memcost > 4*1024*1024) {
        g_set_error (err, validation_error_gquark (), OUTOFRANGE_ERRCODE, "Memory cost must be between 16384 and 4194304.");
        return FALSE;
    }

    for (gint i = 0; i < g_utf8_strlen (new_paral, -1); i++) {
        if (!g_ascii_isdigit(new_paral[i])) {
            g_set_error (err, validation_error_gquark (), NONDIGITS_ERRCODE, "The provided parallelism value contains non digits characters.");
            return FALSE;
        }
    }
    gint32 paral = (gint32)g_ascii_strtoll (new_paral, NULL, 10);
    if (paral < 2 || paral > 128) {
        g_set_error (err, validation_error_gquark (), OUTOFRANGE_ERRCODE, "Parallelism must be between 2 and 128.");
        return FALSE;
    }

    app_data->db_data->argon2id_iter = iter;
    app_data->db_data->argon2id_memcost = memcost;
    app_data->db_data->argon2id_parallelism = paral;

    return TRUE;
}