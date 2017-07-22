#pragma once

#include "kf-misc.h"
#include "treeview.h"

#define APP_NAME "OTPClient"
#define APP_VERSION "0.99.2 (1.0-alpha2)"

#define KF_NAME "otpclient-db.enc"
#define KF_GROUP "data"

void activate (GtkApplication *app, gpointer user_data);

goffset get_file_size (const gchar *path);

void show_message_dialog (GtkWidget *parent, const gchar *message, GtkMessageType message_type);

int add_data_dialog (GtkWidget *main_window, UpdateData *kf_data, GtkListStore *list_store);