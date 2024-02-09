#pragma once

#include "data.h"

G_BEGIN_DECLS

#define QUIT_APP     50
#define RETRY_CHANGE 51
#define CHANGE_OK    52

int  change_file (AppData *app_data);

G_END_DECLS