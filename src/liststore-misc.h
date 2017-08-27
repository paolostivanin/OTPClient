#pragma once

#include "kf-misc.h"

void traverse_liststore (GtkListStore *list_store, UpdateData *kf_data);

void set_otp (GtkListStore *list_store, GtkTreeIter iter, gchar *account_name, UpdateData *kf_data);