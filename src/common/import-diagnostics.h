#pragma once
#include <glib.h>

typedef struct {
    guint skipped_invalid;
    GPtrArray *issues; /* Owned, secret-free descriptions in source order. */
} OtpImportDiagnostics;

OtpImportDiagnostics *otp_import_diagnostics_new (void);
void otp_import_diagnostics_free (OtpImportDiagnostics *diagnostics);
/* The formatted list is shown to the user, so `reason` should be translated.
 * The one exception is a GError message forwarded from otp-validation.c: those
 * name a field and a bound ("digits must be 6, 7 or 8") and are deliberately
 * left in English, matching what the same check prints on the CLI. */
void otp_import_diagnostics_add (OtpImportDiagnostics *diagnostics,
                                 guint source_index, const gchar *reason);
gchar *otp_import_diagnostics_format (const OtpImportDiagnostics *diagnostics);
G_DEFINE_AUTOPTR_CLEANUP_FUNC (OtpImportDiagnostics, otp_import_diagnostics_free)
