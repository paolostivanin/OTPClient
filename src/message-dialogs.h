#pragma once

#include <glib.h>

G_BEGIN_DECLS

void     show_message_dialog            (GtkWidget *parent,
                                         const gchar *message,
                                         GtkMessageType message_type);

gboolean get_confirmation_from_dialog   (GtkWidget *parent,
                                         const gchar *message);

G_END_DECLS