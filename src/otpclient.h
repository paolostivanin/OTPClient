#pragma once

#define APP_NAME "OTPClient"
#define APP_VERSION "1.0.0-alpha"

#define KF_NAME "otpclient-db.enc"
#define KF_GROUP "data"

void activate (GtkApplication *app, gpointer user_data);
void show_message_dialog (GtkWidget *parent, const gchar *message, GtkMessageType message_type);