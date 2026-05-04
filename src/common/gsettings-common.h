#pragma once

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define OTPCLIENT_SEARCH_KEYWORD_MAX_LEN 32

typedef struct {
    gchar *name;
    gchar *path;
} DbListEntry;

/* Caller owns the returned GSettings (free with g_object_unref), the
 * returned gchar pointers (free with g_free), and the returned GPtrArray
 * (free with g_ptr_array_unref). NULL is returned when the GSettings schema
 * is unavailable; the *_string getters also return NULL when no value is
 * configured anywhere (settings + key file). */
GSettings  *gsettings_common_get_settings                (void);

gchar      *gsettings_common_get_db_path                 (void);

gboolean    gsettings_common_get_use_secret_service      (void);

gboolean    gsettings_common_get_search_provider_enabled (void);

gchar      *gsettings_common_get_search_provider_keyword (void);

GPtrArray  *gsettings_common_get_db_list                 (void);

void        db_list_entry_free                           (DbListEntry *entry);

G_END_DECLS
