#pragma once

#define APP_NAME "OTPClient"
#define APP_VERSION "1.0.0-alpha"

#define KF_NAME "otpclient-db.enc"
#define KF_GROUP "data"

goffset get_file_size (const gchar *path);

void activate (GtkApplication *app, gpointer user_data);

void show_message_dialog (GtkWidget *parent, const gchar *message, GtkMessageType message_type);

GtkWidget *create_scrolled_window_with_treeview (GtkWidget *main_window, gchar *decrypted_keyfile, gchar *pwd);