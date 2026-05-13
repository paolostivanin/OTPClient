#pragma once

#include <glib-object.h>
#include <gtk/gtk.h>

G_BEGIN_DECLS

#define DATABASE_TYPE_ENTRY (database_entry_get_type ())

G_DECLARE_FINAL_TYPE (DatabaseEntry, database_entry, DATABASE, ENTRY, GObject)

DatabaseEntry  *database_entry_new            (const gchar *name,
                                              const gchar *path);

const gchar    *database_entry_get_name      (DatabaseEntry *self);
const gchar    *database_entry_get_path      (DatabaseEntry *self);

void            database_entry_set_name      (DatabaseEntry *self,
                                              const gchar   *name);

gboolean        database_entry_get_primary   (DatabaseEntry *self);
void            database_entry_set_primary   (DatabaseEntry *self,
                                              gboolean       is_primary);

gboolean        database_entry_get_missing   (DatabaseEntry *self);
void            database_entry_set_missing   (DatabaseEntry *self,
                                              gboolean       missing);

gboolean        database_entry_get_active    (DatabaseEntry *self);
void            database_entry_set_active    (DatabaseEntry *self,
                                              gboolean       active);

G_END_DECLS
