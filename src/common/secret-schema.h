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