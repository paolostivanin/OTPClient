#pragma once

#include <libsecret/secret.h>

const SecretSchema *otpclient_get_schema (void) G_GNUC_CONST;

#define OTPCLIENT_SCHEMA  otpclient_get_schema ()

void on_password_stored  (GObject      *source,
                          GAsyncResult *result,
                          gpointer      unused);

void on_password_cleared (GObject      *source,
                          GAsyncResult *result,
                          gpointer      unused);

/* Synchronous round-trip test of the registered Secret Service provider:
 * store a probe value, look it up, verify it, then clear it. Uses a sentinel
 * attribute value so the probe cannot collide with a real db_path. Returns
 * TRUE iff every step succeeds. On failure, *error is set to the underlying
 * libsecret error (or a synthetic one for the verify-mismatch case). */
gboolean otpclient_secret_service_probe (GError **error);