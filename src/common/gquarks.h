#pragma once

#include <glib.h>

G_BEGIN_DECLS

#define MISSING_FILE_ERRCODE    10
#define BAD_TAG_ERRCODE         11
#define KEY_DERIVATION_ERRCODE  12
#define FILE_TOO_BIG_ERRCODE    13
#define GENERIC_ERRCODE         14
#define MEMLOCK_ERRCODE         15
#define SECMEM_ALLOC_ERRCODE    16
#define NONDIGITS_ERRCODE       17
#define OUTOFRANGE_ERRCODE      18
#define NO_SECMEM_AVAIL_ERRCODE 19
#define FILE_SIZE_SECMEM_MSG    "Selected file is too big. Please increase the secure memory size."

GQuark missing_file_gquark       (void);

GQuark bad_tag_gquark            (void);

GQuark key_deriv_gquark          (void);

GQuark file_too_big_gquark       (void);

GQuark generic_error_gquark      (void);

GQuark memlock_error_gquark      (void);

GQuark secmem_alloc_error_gquark (void);

GQuark validation_error_gquark (void);

G_END_DECLS
