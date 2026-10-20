#define _DEFAULT_SOURCE
#include <glib/gi18n.h>
#include <adwaita.h>
#include <string.h>
#include "otpclient-application.h"
#include "otpclient-window-private.h"
#include "db-common.h"

typedef struct {
    GWeakRef window_ref;
    DatabaseData *origin_db;
    guint lock_generation;
    json_t *token;
    json_t *batch_snapshot;
    guint position;
} UndoDeleteData;

static void on_undo_delete (AdwToast *toast, gpointer user_data);

static void
undo_delete_token_clear (UndoDeleteData *data)
{
    if (data->token != NULL) {
        json_t *secret = json_object_get (data->token, "secret");
        if (json_is_string (secret)) {
            const char *value = json_string_value (secret);
            size_t len = json_string_length (secret);
            if (value != NULL && len > 0)
                explicit_bzero ((void *) value, len);
        }
        json_decref (data->token);
        data->token = NULL;
    }
    if (data->batch_snapshot != NULL) {
        size_t i;
        json_t *item;
        json_array_foreach (data->batch_snapshot, i, item) {
            json_t *token = json_object_get (item, "token");
            json_t *secret = json_object_get (token, "secret");
            if (json_is_string (secret)) {
                const char *value = json_string_value (secret);
                size_t len = json_string_length (secret);
                if (value != NULL && len > 0)
                    explicit_bzero ((void *) value, len);
            }
        }
        json_decref (data->batch_snapshot);
        data->batch_snapshot = NULL;
    }
}

static UndoDeleteData *
undo_delete_data_new (OTPClientWindow *self,
                      DatabaseData *origin_db,
                      json_t *token,
                      guint position)
{
    UndoDeleteData *data = g_new0 (UndoDeleteData, 1);
    g_weak_ref_init (&data->window_ref, self);
    data->origin_db = origin_db;
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    data->lock_generation = app != NULL
        ? otpclient_application_get_lock_generation (app) : 0;
    data->token = json_deep_copy (token);
    data->position = position;
    if (data->token == NULL) {
        g_weak_ref_clear (&data->window_ref);
        g_free (data);
        return NULL;
    }
    self->undo_actions = g_list_prepend (self->undo_actions, data);
    return data;
}

static void
undo_delete_data_free (gpointer user_data, GClosure *closure)
{
    (void) closure;
    UndoDeleteData *data = user_data;
    g_autoptr (OTPClientWindow) self = g_weak_ref_get (&data->window_ref);
    if (self != NULL)
        self->undo_actions = g_list_remove (self->undo_actions, data);
    g_weak_ref_clear (&data->window_ref);
    undo_delete_token_clear (data);
    g_free (data);
}

void
otpclient_window_undo_show_single (OTPClientWindow *self,
                                   DatabaseData *origin_db,
                                   json_t *token,
                                   guint position)
{
    UndoDeleteData *data = undo_delete_data_new (self, origin_db, token, position);
    if (data == NULL)
        return;
    AdwToast *toast = adw_toast_new (_("Token deleted"));
    adw_toast_set_button_label (toast, _("Undo"));
    adw_toast_set_timeout (toast, 5);
    g_signal_connect_data (toast, "button-clicked", G_CALLBACK (on_undo_delete),
                           data, undo_delete_data_free, 0);
    adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay), toast);
}

void
otpclient_window_undo_show_batch (OTPClientWindow *self,
                                  DatabaseData *origin_db,
                                  json_t *snapshot)
{
    UndoDeleteData *data = g_new0 (UndoDeleteData, 1);
    g_weak_ref_init (&data->window_ref, self);
    data->origin_db = origin_db;
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    data->lock_generation = app != NULL
        ? otpclient_application_get_lock_generation (app) : 0;
    data->batch_snapshot = json_deep_copy (snapshot);
    if (data->batch_snapshot == NULL) {
        g_weak_ref_clear (&data->window_ref);
        g_free (data);
        return;
    }
    self->undo_actions = g_list_prepend (self->undo_actions, data);
    AdwToast *toast = adw_toast_new (_("Tokens deleted"));
    adw_toast_set_button_label (toast, _("Undo"));
    adw_toast_set_timeout (toast, 8);
    g_signal_connect_data (toast, "button-clicked", G_CALLBACK (on_undo_delete),
                           data, undo_delete_data_free, 0);
    adw_toast_overlay_add_toast (ADW_TOAST_OVERLAY (self->toast_overlay), toast);
}

void
otpclient_window_undo_clear (OTPClientWindow *self)
{
    for (GList *l = self->undo_actions; l != NULL; l = l->next)
        undo_delete_token_clear (l->data);
}

static void
on_undo_delete (AdwToast *toast, gpointer user_data)
{
    (void) toast;
    UndoDeleteData *undo = user_data;
    if (undo->token == NULL && undo->batch_snapshot == NULL)
        return;
    g_autoptr (OTPClientWindow) self = g_weak_ref_get (&undo->window_ref);
    if (self == NULL)
        return;

    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    if (app == NULL)
        return;
    DatabaseData *db_data = otpclient_application_get_db_data (app);
    if (db_data == NULL || db_data != undo->origin_db ||
        db_data->in_memory_json_data == NULL ||
        otpclient_application_get_lock_generation (app) != undo->lock_generation)
        return;

    if (undo->batch_snapshot != NULL) {
        GError *err = NULL;
        if (!db_restore_tokens (db_data, undo->batch_snapshot, &err)) {
            g_autofree gchar *message = g_strdup_printf (
                _("Failed to restore deleted tokens: %s"),
                err != NULL ? err->message : _("database write failed"));
            otpclient_window_show_error_toast (self, message);
            g_clear_error (&err);
            otpclient_window_undo_show_batch (self, undo->origin_db,
                                               undo->batch_snapshot);
            return;
        }
        undo_delete_token_clear (undo);
        otpclient_window_database_modified (self);
        return;
    }

    gsize arr_len = json_array_size (db_data->in_memory_json_data);
    guint insert_pos = (undo->position <= arr_len) ? undo->position : (guint) arr_len;
    if (json_array_insert (db_data->in_memory_json_data, insert_pos, undo->token) != 0)
        return;
    GError *err = NULL;
    update_db (db_data, &err);
    if (err != NULL) {
        json_array_remove (db_data->in_memory_json_data, insert_pos);
        g_autofree gchar *message = g_strdup_printf (
            _("Failed to restore deleted token: %s"), err->message);
        otpclient_window_show_error_toast (self, message);
        g_clear_error (&err);
        otpclient_window_undo_show_single (self, undo->origin_db,
                                            undo->token, undo->position);
        return;
    }
    json_decref (undo->token);
    undo->token = NULL;
    otpclient_window_database_modified (self);
}

void
otpclient_window_undo_latest (OTPClientWindow *self)
{
    g_return_if_fail (OTPCLIENT_IS_WINDOW (self));
    if (self->undo_actions != NULL)
        on_undo_delete (NULL, self->undo_actions->data);
}

typedef struct {
    GWeakRef window_ref;
    DatabaseData *db_data;
    guint lock_generation;
    GArray *indices;
} BulkDeleteContext;

static void
bulk_delete_context_free (BulkDeleteContext *ctx)
{
    g_weak_ref_clear (&ctx->window_ref);
    database_data_unref (ctx->db_data);
    g_array_unref (ctx->indices);
    g_free (ctx);
}

static void
on_delete_response (AdwAlertDialog *dialog, GAsyncResult *result,
                    gpointer user_data)
{
    OTPClientWindow *self = OTPCLIENT_WINDOW (user_data);
    const gchar *response = adw_alert_dialog_choose_finish (dialog, result);
    if (g_strcmp0 (response, "delete") != 0)
        return;

    guint pos = otpclient_window_selected_writable_json_index (self);
    if (pos == GTK_INVALID_LIST_POSITION)
        return;
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    DatabaseData *db_data = app != NULL
        ? otpclient_application_get_db_data (app) : NULL;
    if (db_data == NULL || db_data->in_memory_json_data == NULL)
        return;

    json_t *deleted_token = json_deep_copy (
        json_array_get (db_data->in_memory_json_data, pos));
    if (deleted_token == NULL)
        return;
    json_array_remove (db_data->in_memory_json_data, pos);
    GError *err = NULL;
    update_db (db_data, &err);
    if (err != NULL) {
        g_autofree gchar *message = g_strdup_printf (
            _("Failed to delete token: %s"), err->message);
        otpclient_window_show_error_toast (self, message);
        g_clear_error (&err);
        json_array_insert (db_data->in_memory_json_data, pos, deleted_token);
        json_decref (deleted_token);
        return;
    }
    otpclient_window_database_modified (self);
    otpclient_window_undo_show_single (self, db_data, deleted_token, pos);
    json_decref (deleted_token);
}

void
otpclient_window_actions_delete (OTPClientWindow *self)
{
    if (otpclient_window_selected_writable_json_index (self) ==
        GTK_INVALID_LIST_POSITION)
        return;
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG (
        adw_alert_dialog_new (_("Delete Token?"),
                              _("This action cannot be undone.")));
    adw_alert_dialog_add_responses (dialog, "cancel", _("Cancel"),
                                    "delete", _("Delete"), NULL);
    adw_alert_dialog_set_response_appearance (dialog, "delete",
                                               ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response (dialog, "cancel");
    adw_alert_dialog_choose (dialog, GTK_WIDGET (self), NULL,
                             (GAsyncReadyCallback) on_delete_response, self);
}

static void
on_bulk_delete_response (AdwAlertDialog *dialog, GAsyncResult *result,
                         BulkDeleteContext *ctx)
{
    const gchar *response = adw_alert_dialog_choose_finish (dialog, result);
    g_autoptr (OTPClientWindow) self = g_weak_ref_get (&ctx->window_ref);
    OTPClientApplication *app = self != NULL ? OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self))) : NULL;
    if (g_strcmp0 (response, "delete") != 0 || app == NULL || self == NULL ||
        otpclient_application_get_app_locked (app) ||
        otpclient_application_get_db_data (app) != ctx->db_data ||
        otpclient_application_get_lock_generation (app) != ctx->lock_generation) {
        bulk_delete_context_free (ctx);
        return;
    }
    json_t *snapshot = NULL;
    GError *err = NULL;
    if (!db_delete_tokens (ctx->db_data, ctx->indices, &snapshot, &err)) {
        g_autofree gchar *message = g_strdup_printf (
            _("Could not delete selected tokens: %s"),
            err != NULL ? err->message : _("database write failed"));
        otpclient_window_show_error_toast (self, message);
        g_clear_error (&err);
        bulk_delete_context_free (ctx);
        return;
    }
    otpclient_window_database_modified (self);
    otpclient_window_undo_show_batch (self, ctx->db_data, snapshot);
    json_decref (snapshot);
    bulk_delete_context_free (ctx);
}

void
otpclient_window_actions_bulk_delete (OTPClientWindow *self)
{
    OTPClientApplication *app = OTPCLIENT_APPLICATION (
        gtk_window_get_application (GTK_WINDOW (self)));
    DatabaseData *db_data = app != NULL
        ? otpclient_application_get_db_data (app) : NULL;
    if (db_data == NULL)
        return;
    GArray *indices = otpclient_window_get_bulk_selected_indices (self, NULL);
    if (indices->len == 0) {
        g_array_unref (indices);
        return;
    }
    BulkDeleteContext *ctx = g_new0 (BulkDeleteContext, 1);
    g_weak_ref_init (&ctx->window_ref, self);
    ctx->db_data = database_data_ref (db_data);
    ctx->lock_generation = otpclient_application_get_lock_generation (app);
    ctx->indices = indices;
    g_autofree gchar *message = g_strdup_printf (
        ngettext ("Delete %u selected token? You can undo this action.",
                  "Delete %u selected tokens? You can undo this action.",
                  indices->len), indices->len);
    AdwAlertDialog *dialog = ADW_ALERT_DIALOG (
        adw_alert_dialog_new (_("Delete Selected Tokens?"), message));
    adw_alert_dialog_add_responses (dialog, "cancel", _("Cancel"),
                                    "delete", _("Delete"), NULL);
    adw_alert_dialog_set_response_appearance (dialog, "delete",
                                               ADW_RESPONSE_DESTRUCTIVE);
    adw_alert_dialog_set_default_response (dialog, "cancel");
    adw_alert_dialog_choose (dialog, GTK_WIDGET (self), NULL,
        (GAsyncReadyCallback) on_bulk_delete_response, ctx);
}
