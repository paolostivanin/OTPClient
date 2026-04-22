#pragma once

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define OTPCLIENT_SEARCH_KEYWORD_MAX_LEN 32

typedef struct {
    gchar *name;
    gchar *path;
} DbListEntry;

GSettings  *gsettings_common_get_settings                (void);

gchar      *gsettings_common_get_db_path                 (void);

gboolean    gsettings_common_get_use_secret_service      (void);

gboolean    gsettings_common_get_search_provider_enabled (void);

gchar      *gsettings_common_get_search_provider_keyword (void);

GPtrArray  *gsettings_common_get_db_list                 (void);

void        db_list_entry_free                           (DbListEntry *entry);

G_END_DECLS
