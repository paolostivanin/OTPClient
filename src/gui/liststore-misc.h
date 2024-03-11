#pragma once

G_BEGIN_DECLS

gboolean traverse_liststore (gpointer        user_data);

void     set_otp            (GtkListStore   *list_store,
                             GtkTreeIter     iter,
                             AppData        *app_data);

G_END_DECLS
