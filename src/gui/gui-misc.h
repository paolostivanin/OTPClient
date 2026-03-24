#pragma once

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

gchar   *gui_misc_get_db_path_from_cfg   (void);

void     gui_misc_save_db_path_to_cfg    (const gchar *db_path);

GPtrArray *gui_misc_get_db_list          (void);

void     gui_misc_save_db_list           (GListStore *db_store);

gboolean gui_misc_add_db_to_list         (GListStore  *db_store,
                                          const gchar *name,
                                          const gchar *path);

void     gui_misc_remove_db_from_list    (GListStore *db_store,
                                          guint       index);

void     gui_misc_rename_db_in_list      (GListStore  *db_store,
                                          guint        index,
                                          const gchar *new_name);

gchar   *gui_misc_derive_db_display_name (const gchar *path);

void     gui_misc_send_notification      (GApplication *app,
                                          const gchar  *title,
                                          const gchar  *body);

G_END_DECLS
