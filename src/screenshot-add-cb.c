#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <zbar.h>
#include <png.h>
#include <gcrypt.h>
#include "imports.h"
#include "message-dialogs.h"
#include "gui-common.h"
#include "add-common.h"
#include "qrcode-parser.h"


void
screenshot_cb (GSimpleAction *simple    __attribute__((unused)),
               GVariant      *parameter __attribute__((unused)),
               gpointer       user_data)
{
    AppData *app_data = (AppData *)user_data;

    GError *err = NULL;
    GDBusConnection *connection = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &err);
    if (err != NULL) {
        show_message_dialog (app_data->main_window, err->message, GTK_MESSAGE_ERROR);
        g_clear_error (&err);
        return;
    }

    const gchar *interface = "org.gnome.Shell.Screenshot";
    const gchar *object_path = "/org/gnome/Shell/Screenshot";

    GVariant *res = g_dbus_connection_call_sync (connection, interface, object_path, interface,
                                                 "SelectArea", NULL, NULL, G_DBUS_CALL_FLAGS_NONE, G_MAXINT, NULL, &err);
    if (err != NULL) {
        show_message_dialog (app_data->main_window, err->message, GTK_MESSAGE_ERROR);
        g_object_unref (connection);
        g_clear_error (&err);
        return;
    }

    gint x, y, width, height;
    g_variant_get (res, "(iiii)", &x, &y, &width, &height);
    g_variant_unref (res);

#ifndef USE_FLATPAK_APP_FOLDER
    gchar *filename = g_build_filename (g_get_tmp_dir (), "qrcode.png", NULL);
#else
    gchar *filename = g_build_filename (g_get_user_data_dir (), "qrcode.png", NULL);
#endif
    res = g_dbus_connection_call_sync (connection, interface, object_path, interface,
                                       "ScreenshotArea", g_variant_new ("(iiiibs)", x, y, width, height, TRUE, filename),
                                       NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &err);
    if (err != NULL) {
        show_message_dialog (app_data->main_window, err->message, GTK_MESSAGE_ERROR);
        g_clear_error (&err);
        g_free (filename);
        g_object_unref (connection);
        return;
    }

    gboolean success;
    gchar *out_filename;
    g_variant_get (res, "(bs)", &success, &out_filename);
    if (!success) {
        show_message_dialog (app_data->main_window, "Failed to get screenshot using ScreenshotArea", GTK_MESSAGE_ERROR);
        g_variant_unref (res);
        g_free (filename);
        g_object_unref (connection);
        return;
    }

    gchar *otpauth_uri = NULL;
    gchar *err_msg = parse_qrcode (out_filename, &otpauth_uri);
    if (err_msg != NULL) {
        show_message_dialog (app_data->main_window, err_msg, GTK_MESSAGE_ERROR);
        g_free (err_msg);
        g_variant_unref (res);
        g_free (filename);
        g_object_unref (connection);
        return;
    }

    g_unlink (filename);

    err_msg = add_data_to_db (otpauth_uri, app_data);
    if (err_msg != NULL) {
        show_message_dialog (app_data->main_window, err_msg, GTK_MESSAGE_ERROR);
        g_free (err_msg);
        // no need to return here as we have to clean-up also the following stuff
    } else {
        show_message_dialog (app_data->main_window, "QRCode successfully imported from the screenshot", GTK_MESSAGE_INFO);
    }

    gcry_free (otpauth_uri);
    g_variant_unref (res);
    g_free (filename);
    g_object_unref (connection);
}
