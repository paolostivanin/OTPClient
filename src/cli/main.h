#pragma once

#include <glib.h>
#include "../gui/data.h"

G_BEGIN_DECLS

#define MAX_ABS_PATH_LEN 256

typedef struct cmdline_opts_t {
    gchar *database;
    gboolean show;
    gchar *account;
    gchar *issuer;
    gboolean match_exact;
    gboolean show_next;
    gboolean list;
    gboolean export;
    gchar *export_type;
    gchar *export_dir;
} CmdlineOpts;

gboolean exec_action (CmdlineOpts  *cmdline_opts,
                      DatabaseData *db_data);

void     free_dbdata (DatabaseData *db_data);

G_END_DECLS