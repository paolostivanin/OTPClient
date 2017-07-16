#pragma once

#include "kf-misc.h"

#define APP_NAME "OTPClient"
#define APP_VERSION "0.99.1 (1.0-alpha1)"

#define KF_NAME "otpclient-db.enc"
#define KF_GROUP "data"

goffset get_file_size (const gchar *path);

void show_message_dialog (GtkWidget *parent, const gchar *message, GtkMessageType message_type);

int add_data_dialog (GtkWidget *main_window, UpdateData *kf_data);