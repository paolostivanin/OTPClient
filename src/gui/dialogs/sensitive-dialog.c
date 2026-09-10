#define _DEFAULT_SOURCE
#include <string.h>
#include "sensitive-dialog.h"

typedef struct {
    gboolean closed;
    GCancellable *cancellable;
    SensitiveDialogClearFunc clear;
} SensitiveDialogState;

static void
state_free (gpointer data)
{
    SensitiveDialogState *state = data;
    g_object_unref (state->cancellable);
    g_free (state);
}

static void
clear_widget (GtkWidget *widget)
{
    if (GTK_IS_SPIN_BUTTON (widget))
        return;
    if (GTK_IS_EDITABLE (widget)) {
        const gchar *text = gtk_editable_get_text (GTK_EDITABLE (widget));
        if (text != NULL)
            explicit_bzero ((gchar *) text, strlen (text));
        gtk_editable_set_text (GTK_EDITABLE (widget), "");
        return; /* Do not visit the editable's delegate a second time. */
    }
    if (GTK_IS_PICTURE (widget))
        gtk_picture_set_paintable (GTK_PICTURE (widget), NULL);
    for (GtkWidget *child = gtk_widget_get_first_child (widget); child != NULL;
         child = gtk_widget_get_next_sibling (child))
        clear_widget (child);
}

static void
on_closed (AdwDialog *dialog, gpointer data)
{
    (void) data;
    sensitive_dialog_clear (dialog);
}

void
sensitive_dialog_clear (AdwDialog *dialog)
{
    SensitiveDialogState *state = g_object_get_data (G_OBJECT (dialog), "sensitive-state");
    if (state == NULL || state->closed)
        return;
    state->closed = TRUE;
    g_cancellable_cancel (state->cancellable);
    clear_widget (GTK_WIDGET (dialog));
    if (state->clear != NULL)
        state->clear (dialog);
}

void
sensitive_dialog_setup (AdwDialog *dialog, SensitiveDialogClearFunc clear)
{
    SensitiveDialogState *state = g_new0 (SensitiveDialogState, 1);
    state->cancellable = g_cancellable_new ();
    state->clear = clear;
    g_object_set_data_full (G_OBJECT (dialog), "sensitive-state", state, state_free);
    g_signal_connect (dialog, "closed", G_CALLBACK (on_closed), NULL);
}

gboolean
sensitive_dialog_is_closed (AdwDialog *dialog)
{
    SensitiveDialogState *state = g_object_get_data (G_OBJECT (dialog), "sensitive-state");
    return state == NULL || state->closed;
}

GCancellable *
sensitive_dialog_get_cancellable (AdwDialog *dialog)
{
    SensitiveDialogState *state = g_object_get_data (G_OBJECT (dialog), "sensitive-state");
    return state != NULL ? state->cancellable : NULL;
}
