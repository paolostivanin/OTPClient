#include <glib.h>
#include <string.h>
#include <gcrypt.h>
#include <termios.h>
#include <libsecret/secret.h>
#include <glib/gi18n.h>
#include "help.h"
#include "get-data.h"
#include "../common/common.h"
#include "../common/exports.h"
#include "../secret-schema.h"

#define MAX_ABS_PATH_LEN 256

#ifndef USE_FLATPAK_APP_FOLDER
static gchar    *get_db_path              (void);
#endif

static gchar    *get_pwd                  (const gchar *pwd_msg);

static gboolean  is_secretservice_disable (void);


gint
main (gint    argc,
      gchar **argv)
{
    if (show_help (argv[0], argv[1])) {
        return 0;
    }

    DatabaseData *db_data = g_new0 (DatabaseData, 1);
    db_data->key_stored = FALSE;

    db_data->max_file_size_from_memlock = get_max_file_size_from_memlock ();
    gchar *init_msg = init_libs (db_data->max_file_size_from_memlock);
    if (init_msg != NULL) {
        g_printerr ("%s\n", init_msg);
        g_free (init_msg);
        g_free (db_data);
        return -1;
    }

#ifdef USE_FLATPAK_APP_FOLDER
    db_data->db_path = g_build_filename (g_get_user_data_dir (), "otpclient-db.enc", NULL);
    // on the first run the cfg file is not created in the flatpak version because we use a non-changeable db path
    gchar *cfg_file_path = g_build_filename (g_get_user_data_dir (), "otpclient.cfg", NULL);
    if (!g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
        g_file_set_contents (cfg_file_path, "[config]", -1, NULL);
    }
    g_free (cfg_file_path);
#else
    db_data->db_path = get_db_path ();
    if (db_data->db_path == NULL) {
        g_free (db_data);
        return -1;
    }
#endif

    gboolean disable_secret_service = is_secretservice_disable ();
    if (disable_secret_service == FALSE) {
        gchar *pwd = secret_password_lookup_sync (OTPCLIENT_SCHEMA, NULL, NULL, "string", "main_pwd", NULL);
        if (pwd == NULL) {
            goto get_pwd;
        } else {
            db_data->key_stored = TRUE;
            db_data->key= secure_strdup (pwd);
            secret_password_free (pwd);
        }
    } else {
        get_pwd:
        db_data->key = get_pwd (_("Type the DB decryption password: "));
        if (db_data->key == NULL) {
            g_free (db_data);
            return -1;
        }
    }

    db_data->objects_hash = NULL;

    GError *err = NULL;
    load_db (db_data, &err);
    if (err != NULL) {
        const gchar *tmp_msg = _("Error while loading the database:");
        gchar *msg = g_strconcat (tmp_msg, " %s\n", err->message, NULL);
        g_printerr ("%s\n", msg);
        gcry_free (db_data->key);
        g_free (db_data);
        g_free (msg);
        return -1;
    }

    if (disable_secret_service == FALSE && db_data->key_stored == FALSE) {
        secret_password_store (OTPCLIENT_SCHEMA, SECRET_COLLECTION_DEFAULT, "main_pwd", db_data->key, NULL, on_password_stored, NULL, "string", "main_pwd", NULL);
    }

    gchar *account = NULL, *issuer = NULL;
    gboolean show_next_token = FALSE, match_exactly = FALSE;

    if (g_strcmp0 (argv[1], "show") == 0) {
        if (argc < 4 || argc > 8) {
            // Translators: please do not translate '%s --help-show'
            g_printerr (_("Wrong argument(s). Please type '%s --help-show' to see the available options.\n"), argv[0]);
            g_free (db_data);
            return -1;
        }
        for (gint i = 2; i < argc; i++) {
            if (g_strcmp0 (argv[i], "-a") == 0) {
                account = argv[i + 1];
            } else if (g_strcmp0 (argv[i], "-i") == 0) {
                issuer = argv[i + 1];
            } else if (g_strcmp0 (argv[i], "-m") == 0) {
                match_exactly = TRUE;
            } else if (g_strcmp0 (argv[i], "-n") == 0) {
                show_next_token = TRUE;
            }
        }
        if (account == NULL) {
            // Translators: please do not translate 'account'
            g_printerr ("%s\n", _("[ERROR]: The account option (-a) must be specified and can not be empty."));
            goto end;
        }
        show_token (db_data, account, issuer, match_exactly, show_next_token);
    } else if (g_strcmp0 (argv[1], "list") == 0) {
        list_all_acc_iss (db_data);
    } else if (g_strcmp0 (argv[1], "export") == 0) {
        if (g_ascii_strcasecmp (argv[3], "andotp_plain") != 0 && g_ascii_strcasecmp (argv[3], "andotp_encrypted") != 0 &&
            g_ascii_strcasecmp (argv[3], "freeotpplus") != 0 && g_ascii_strcasecmp (argv[3], "aegis") != 0) {
                // Translators: please do not translate '%s --help-export'
                g_printerr (_("Wrong argument(s). Please type '%s --help-export' to see the available options.\n"), argv[0]);
                g_free (db_data);
                return -1;
        }
        const gchar *base_dir = NULL;
#ifndef USE_FLATPAK_APP_FOLDER
        if (argv[4] == NULL) {
            base_dir = g_get_home_dir ();
        } else {
            if (g_ascii_strcasecmp (argv[4], "-d") == 0 && argv[5] != NULL) {
                if (!g_file_test (argv[5], G_FILE_TEST_IS_DIR)) {
                    g_printerr (_("%s is not a directory or the folder doesn't exist. The output will be saved into the HOME directory.\n"), argv[5]);
                    base_dir = g_get_home_dir ();
                } else {
                    base_dir = argv[5];
                }
            } else {
                g_printerr ("%s\n", _("Incorrect parameters used for setting the output folder. Therefore, the exported file will be saved into the HOME directory."));
                base_dir = g_get_home_dir ();
            }
        }
#else
        base_dir = g_get_user_data_dir ();
#endif
        gchar *andotp_export_pwd = NULL, *exported_file_path = NULL, *ret_msg = NULL;
        if (g_ascii_strcasecmp (argv[3], "andotp_plain") == 0 || g_ascii_strcasecmp (argv[3], "andotp_encrypted") == 0) {
            if (g_ascii_strcasecmp (argv[3], "andotp_encrypted")) {
                andotp_export_pwd = get_pwd (_("Type the export encryption password: "));
                if (andotp_export_pwd == NULL) {
                    goto end;
                }
            }
            exported_file_path = g_build_filename (base_dir, andotp_export_pwd != NULL ? "andotp_exports.json.aes" : "andotp_exports.json", NULL);
            ret_msg = export_andotp (exported_file_path, andotp_export_pwd, db_data->json_data);
            gcry_free (andotp_export_pwd);
        }
        if (g_ascii_strcasecmp (argv[3], "freeotpplus") == 0) {
            exported_file_path = g_build_filename (base_dir, "freeotpplus-exports.txt", NULL);
            ret_msg = export_freeotpplus (exported_file_path, db_data->json_data);
        }
        if (g_ascii_strcasecmp (argv[3], "aegis") == 0) {
            exported_file_path = g_build_filename (base_dir, "aegis_export_plain.json", NULL);
            ret_msg = export_aegis (exported_file_path, db_data->json_data, NULL);
        }
        if (ret_msg != NULL) {
            g_printerr (_("An error occurred while exporting the data: %s\n"), ret_msg);
            g_free (ret_msg);
        } else {
            g_print (_("Data successfully exported to: %s\n"), exported_file_path);
        }
        g_free (exported_file_path);
    } else {
        show_help (argv[0], "help");
        return -1;
    }

    end:
    gcry_free (db_data->key);
    g_free (db_data->db_path);
    g_slist_free_full (db_data->objects_hash, g_free);
    json_decref (db_data->json_data);
    g_free (db_data);

    return 0;
}


#ifndef USE_FLATPAK_APP_FOLDER
static gchar *
get_db_path (void)
{
    gchar *db_path = NULL;
    GError *err = NULL;
    GKeyFile *kf = g_key_file_new ();
    gchar *cfg_file_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
    if (g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
        if (!g_key_file_load_from_file (kf, cfg_file_path, G_KEY_FILE_NONE, &err)) {
            g_printerr ("%s\n", err->message);
            g_key_file_free (kf);
            g_clear_error (&err);
            return NULL;
        }
        db_path = g_key_file_get_string (kf, "config", "db_path", NULL);
        if (db_path == NULL) {
            goto type_db_path;
        }
        if (!g_file_test (db_path, G_FILE_TEST_EXISTS)) {
            gchar *msg = g_strconcat ("Database file/location (", db_path, ") does not exist.\n", NULL);
            g_printerr ("%s\n", msg);
            g_free (msg);
            goto type_db_path;
        }
        goto end;
    }
    type_db_path: ; // empty statement workaround
    g_print ("%s", _("Type the absolute path to the database: "));
    db_path = g_malloc0 (MAX_ABS_PATH_LEN);
    if (fgets (db_path, MAX_ABS_PATH_LEN, stdin) == NULL) {
        g_printerr ("%s\n", _("Couldn't get db path from stdin"));
        g_free (cfg_file_path);
        g_free (db_path);
        return NULL;
    } else {
        // remove the newline char
        db_path[g_utf8_strlen (db_path, -1) - 1] = '\0';
        if (!g_file_test (db_path, G_FILE_TEST_EXISTS)) {
            g_printerr (_("File '%s' does not exist\n"), db_path);
            g_free (cfg_file_path);
            g_free (db_path);
            return NULL;
        }
    }

    end:
    g_free (cfg_file_path);

    return db_path;
}
#endif


static gchar *
get_pwd (const gchar *pwd_msg)
{
    gchar *pwd = gcry_calloc_secure (256, 1);
    g_print ("%s", pwd_msg);

    struct termios old, new;
    if (tcgetattr (STDIN_FILENO, &old) != 0) {
        g_printerr ("%s\n", _("Couldn't get termios info"));
        gcry_free (pwd);
        return NULL;
    }
    new = old;
    new.c_lflag &= ~ECHO;
    if (tcsetattr (STDIN_FILENO, TCSAFLUSH, &new) != 0) {
        g_printerr ("%s\n", _("Couldn't turn echoing off"));
        gcry_free (pwd);
        return NULL;
    }
    if (fgets (pwd, 256, stdin) == NULL) {
        g_printerr ("%s\n", _("Couldn't read password from stdin"));
        gcry_free (pwd);
        return NULL;
    }
    g_print ("\n");
    tcsetattr (STDIN_FILENO, TCSAFLUSH, &old);

    pwd[g_utf8_strlen (pwd, -1) - 1] = '\0';

    gchar *realloc_pwd = gcry_realloc (pwd, g_utf8_strlen (pwd, -1) + 1);

    return realloc_pwd;
}


static gboolean
is_secretservice_disable (void)
{
    gboolean disable_secret_service = FALSE;
    GError *err = NULL;
    GKeyFile *kf = g_key_file_new ();
    gchar *cfg_file_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
    if (g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
        if (!g_key_file_load_from_file (kf, cfg_file_path, G_KEY_FILE_NONE, &err)) {
            g_printerr ("%s\n", err->message);
            g_key_file_free (kf);
            g_clear_error (&err);
            return FALSE;
        }
        disable_secret_service = g_key_file_get_boolean (kf, "config", "disable_secret_service", NULL);
    }
    return disable_secret_service;
}
