#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define MISSING_FILE_CODE       10
#define BAD_TAG_ERRCODE         11
#define KEY_DERIVATION_ERRCODE  12
#define FILE_TOO_BIG            13
#define GENERIC_ERRCODE         14
#define MEMLOCK_ERRCODE         15

GQuark missing_file_gquark   (void);

GQuark bad_tag_gquark        (void);

GQuark key_deriv_gquark      (void);

GQuark file_too_big_gquark   (void);

GQuark generic_error_gquark  (void);

GQuark memlock_error_gquark  (void);

G_END_DECLS