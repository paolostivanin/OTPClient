#include <glib.h>
#include <string.h>
#include <gcrypt.h>
#include <termios.h>
#include "help.h"
#include "get-data.h"
#include "../common/common.h"
#include "../db-misc.h"
#include "../otpclient.h"

#define MAX_ABS_PATH_LEN 256

#ifndef USE_FLATPAK_APP_FOLDER
static gchar    *get_db_path    (void);
#endif

static gchar    *get_pwd        (void);


gint
main (gint    argc,
      gchar **argv)
{
    if (show_help (argv[0], argv[1])) {
        return 0;
    }

    DatabaseData *db_data = g_new0 (DatabaseData, 1);

    db_data->max_file_size_from_memlock = get_max_file_size_from_memlock ();
    gchar *msg = init_libs (db_data->max_file_size_from_memlock);
    if (msg != NULL) {
        g_printerr ("%s\n", msg);
        g_free (msg);
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

    db_data->key = get_pwd ();
    if (db_data->key == NULL) {
        g_free (db_data);
        return -1;
    }

    db_data->objects_hash = NULL;

    GError *err = NULL;
    load_db (db_data, &err);
    if (err != NULL) {
        g_printerr ("Error while loading the database: %s\n", err->message);
        gcry_free (db_data->key);
        g_free (db_data);
        return -1;
    }

    gchar *account = NULL, *issuer = NULL;
    gboolean show_next_token = FALSE, match_exactly = FALSE;

    if (g_strcmp0 (argv[1], "show") == 0) {
        if (argc < 4 || argc > 8) {
            g_printerr ("Wrong argument(s). Please type '%s --help-show' to see the available options.\n", argv[0]);
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
            g_printerr ("[ERROR]: The account option (-a) must be specified and can not be empty.\n");
            goto end;
        }
        show_token (db_data, account, issuer, match_exactly, show_next_token);
    } else if (g_strcmp0 (argv[1], "list") == 0) {
        list_all_acc_iss (db_data);
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
get_db_path ()
{
    gchar *db_path = NULL;
    GError *err = NULL;
    GKeyFile *kf = g_key_file_new ();
    gchar *cfg_file_path = g_build_filename (g_get_user_config_dir (), "otpclient.cfg", NULL);
    if (g_file_test (cfg_file_path, G_FILE_TEST_EXISTS)) {
        if (!g_key_file_load_from_file (kf, cfg_file_path, G_KEY_FILE_NONE, &err)) {
            g_printerr ("%s\n", err->message);
            g_key_file_free (kf);
            return NULL;
        }
        db_path = g_key_file_get_string (kf, "config", "db_path", &err);
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
    g_print ("Type the absolute path to the database: ");
    db_path = g_malloc0 (MAX_ABS_PATH_LEN);
    if (fgets (db_path, MAX_ABS_PATH_LEN, stdin) == NULL) {
        g_printerr ("Couldn't get db path from stdin\n");
        g_free (cfg_file_path);
        return NULL;
    } else {
        // remove the newline char
        db_path[g_utf8_strlen (db_path, -1) - 1] = '\0';
        if (!g_file_test (db_path, G_FILE_TEST_EXISTS)) {
            g_printerr ("File '%s' does not exist\n", db_path);
            g_free (cfg_file_path);
            return NULL;
        }
    }

    end:
    g_free (cfg_file_path);

    return db_path;
}
#endif


static gchar *
get_pwd ()
{
    gchar *pwd = gcry_calloc_secure (256, 1);
    g_print ("Type the password: ");

    struct termios old, new;
    if (tcgetattr (STDIN_FILENO, &old) != 0) {
        g_printerr ("Couldn't get termios info\n");
        gcry_free (pwd);
        return NULL;
    }
    new = old;
    new.c_lflag &= ~ECHO;
    if (tcsetattr (STDIN_FILENO, TCSAFLUSH, &new) != 0) {
        g_printerr ("Couldn't turn echoing off\n");
        gcry_free (pwd);
        return NULL;
    }
    if (fgets (pwd, 256, stdin) == NULL) {
        g_printerr ("Couldn't read password from stdin\n");
        gcry_free (pwd);
        return NULL;
    }
    g_print ("\n");
    tcsetattr (STDIN_FILENO, TCSAFLUSH, &old);

    pwd[g_utf8_strlen (pwd, -1) - 1] = '\0';

    gchar *realloc_pwd = gcry_realloc (pwd, g_utf8_strlen (pwd, -1) + 1);

    return realloc_pwd;
}