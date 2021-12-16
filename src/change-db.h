#pragma once

#include <gtk/gtk.h>

G_BEGIN_DECLS

void change_db_cb (GSimpleAction *simple    __attribute__((unused)),
                   GVariant      *parameter __attribute__((unused)),
                   gpointer       user_data);

G_END_DECLS
