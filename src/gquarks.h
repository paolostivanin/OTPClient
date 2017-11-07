#pragma once

#include <glib.h>

G_BEGIN_DECLS

GQuark missing_file_gquark  (void);

GQuark invalid_input_gquark (void);

GQuark bad_tag_gquark (void);

GQuark key_deriv_gquark (void);

GQuark generic_error_gquark (void);

G_END_DECLS