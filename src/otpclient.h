#pragma once

#include "kf-misc.h"

#define APP_NAME "OTPClient"
#define APP_VERSION "1.0.0-alpha"

#define KF_NAME "otpclient-db.enc"
#define KF_GROUP "data"

goffset get_file_size (const gchar *path);

void set_icon_to_entry (GtkWidget *entry, const gchar *icon_name, const gchar *tooltip_text);

void show_message_dialog (GtkWidget *parent, const gchar *message, GtkMessageType message_type);

void add_data_dialog (GtkWidget *main_window, UpdateData *kf_data);