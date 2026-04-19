#include <glib.h>
#include <glib/gi18n.h>
#include <gcrypt.h>
#include <jansson.h>
#include <termios.h>
#include <libsecret/secret.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "main.h"
#include "get-data.h"
#include "../common/import-export.h"
#include "../common/file-size.h"
#include "../common/secret-schema.h"
#include "../common/gquarks.h"
#include "../common/gsettings-common.h"

static gchar    *resolve_db_path       (const gchar *database_arg);

static gchar    *get_pwd               (const gchar *pwd_msg,
                                        int password_fd);


gboolean exec_action (CmdlineOpts  *cmdline_opts,
                      DatabaseData *db_data)
{
    if (cmdline_opts->list_databases) {
        g_autoptr (GPtrArray) db_list = gsettings_common_get_db_list ();
        if (cmdline_opts->output_format == OUTPUT_FORMAT_JSON) {
            json_t *arr = json_array ();
            if (db_list != NULL) {
                for (guint i = 0; i < db_list->len; i++) {
                    DbListEntry *entry = g_ptr_array_index (db_list, i);
                    json_t *row = json_object ();
                    json_object_set_new (row, "name", json_string (entry->name ? entry->name : ""));
                    json_object_set_new (row, "path", json_string (entry->path ? entry->path : ""));
                    json_array_append_new (arr, row);
                }
            }
            char *dumped = json_dumps (arr, JSON_INDENT (2));
            g_print ("%s\n", dumped);
            gcry_free (dumped);
            json_decref (arr);
            return TRUE;
        }
        if (cmdline_opts->output_format == OUTPUT_FORMAT_CSV) {
            g_print ("name,path\n");
            if (db_list != NULL) {
                for (guint i = 0; i < db_list->len; i++) {
                    DbListEntry *entry = g_ptr_array_index (db_list, i);
                    /* names/paths shouldn't normally contain commas, but escape just in case. */
                    const gchar *n = entry->name ? entry->name : "";
                    const gchar *p = entry->path ? entry->path : "";
                    gboolean quote_n = strpbrk (n, ",\"\r\n") != NULL;
                    gboolean quote_p = strpbrk (p, ",\"\r\n") != NULL;
                    if (quote_n) g_print ("\"%s\",", n); else g_print ("%s,", n);
                    if (quote_p) g_print ("\"%s\"\n", p); else g_print ("%s\n", p);
                }
            }
            return TRUE;
        }
        if (db_list == NULL || db_list->len == 0) {
            g_print ("No databases found in the configuration.\n");
            return TRUE;
        }
        g_print ("Known databases:\n");
        for (guint i = 0; i < db_list->len; i++) {
            DbListEntry *entry = g_ptr_array_index (db_list, i);
            g_print ("  %s  →  %s\n", entry->name, entry->path);
        }
        return TRUE;
    }

    db_data->db_path = resolve_db_path (cmdline_opts->database);
    if (db_data->db_path == NULL) {
        return FALSE;
    }

    if (g_file_test (db_data->db_path, G_FILE_TEST_EXISTS)) {
        if (get_file_size (db_data->db_path) > (goffset)(db_data->max_file_size_from_memlock * SECMEM_SIZE_THRESHOLD_RATIO)) {
            gchar *msg = g_strdup_printf (_(
                "Your system's secure memory limit (memlock: %d bytes) is not enough to securely load the database into memory.\n"
                "You need to increase your system's memlock limit by following the instructions on our "
                "secure memory wiki page: https://github.com/paolostivanin/OTPClient/wiki/Secure-Memory-Limitations\n"
                "This requires administrator privileges and is a system-wide setting that OTPClient cannot change automatically."
            ), db_data->max_file_size_from_memlock);
            g_printerr ("%s\n", msg);
            g_free (msg);
            return FALSE;
        }
    }

    gboolean use_secret_service = gsettings_common_get_use_secret_service ();
    int password_fd = STDIN_FILENO;
    if (cmdline_opts->password_file) {
        if (g_file_test (cmdline_opts->password_file, G_FILE_TEST_IS_SYMLINK)) {
            g_printerr ("Refusing to open password file '%s': it is a symbolic link.\n", cmdline_opts->password_file);
            return FALSE;
        }
        GStatBuf st_before;
        if (g_stat (cmdline_opts->password_file, &st_before) != 0) {
            g_printerr ("Failed to stat password file '%s': %s\n", cmdline_opts->password_file, g_strerror (errno));
            return FALSE;
        }
        password_fd = g_open (cmdline_opts->password_file, O_RDONLY, 0);
        if (password_fd < 0) {
            g_printerr ("Failed to open password file '%s': %s\n", cmdline_opts->password_file, g_strerror (errno));
            return FALSE;
        }
        GStatBuf st_after;
        if (fstat (password_fd, &st_after) == 0) {
            if (st_before.st_ino != st_after.st_ino || st_before.st_dev != st_after.st_dev) {
                g_printerr ("Refusing to open password file '%s': file changed between check and open.\n", cmdline_opts->password_file);
                close (password_fd);
                return FALSE;
            }
            if ((st_after.st_mode & 077) != 0) {
                g_printerr ("Refusing to open password file '%s': mode %04o is readable or writable by group/other. "
                             "Run `chmod 600 %s` and retry.\n",
                             cmdline_opts->password_file,
                             (unsigned)(st_after.st_mode & 0777),
                             cmdline_opts->password_file);
                close (password_fd);
                return FALSE;
            }
        }
    }
    if (use_secret_service == TRUE && g_file_test (db_data->db_path, G_FILE_TEST_EXISTS)) {
        gchar *pwd = secret_password_lookup_sync (OTPCLIENT_SCHEMA, NULL, NULL, "string", db_data->db_path, NULL);
        if (pwd == NULL) {
            goto get_pwd;
        }
        db_data->key_stored = TRUE;
        db_data->key = secure_strdup (pwd);
        secret_password_free (pwd);
    } else {
        get_pwd:
        db_data->key = get_pwd (_("Type the DB decryption password: "), password_fd);
        if (db_data->key == NULL) {
            g_printerr ("%s\n", _("No password provided, exiting."));
            if (password_fd != STDIN_FILENO) {
                close (password_fd);
            }
            return FALSE;
        }
    }
    if (password_fd != STDIN_FILENO) {
        close (password_fd);
    }

    GError *err = NULL;
    if (cmdline_opts->import && !g_file_test (db_data->db_path, G_FILE_TEST_EXISTS)) {
        g_print ("Database file does not exist. Creating a new database...\n");
        update_db (db_data, &err);
        if (err != NULL) {
            g_printerr (_("Error while creating new database: %s\n"), err->message);
            g_clear_error (&err);
            return FALSE;
        }

        g_print ("Database '%s' created successfully.\n", db_data->db_path);
        g_print ("\nATTENTION: if you want to use this database by default, you must update the config file accordingly.\n\n");
    } else {
        load_db (db_data, &err);
        if (err != NULL) {
            if (err->domain == bad_tag_gquark ()) {
                g_printerr ("%s\n", _("Incorrect password."));
            } else {
                gchar *msg = g_strconcat (_("Error while loading the database: "), err->message, NULL);
                g_printerr ("%s\n", msg);
                g_free (msg);
            }
            g_clear_error (&err);
            return FALSE;
        }
    }

    if (use_secret_service == TRUE && db_data->key_stored == FALSE) {
        secret_password_store (OTPCLIENT_SCHEMA, SECRET_COLLECTION_DEFAULT, "OTPClient database password", db_data->key, NULL, on_password_stored, NULL, "string", db_data->db_path, NULL);
    }

    if (cmdline_opts->show) {
        show_token (db_data, cmdline_opts->account, cmdline_opts->issuer,
                    cmdline_opts->match_exact, cmdline_opts->show_next,
                    cmdline_opts->output_format);
    }

    if (cmdline_opts->list) {
        list_all_acc_iss (db_data, cmdline_opts->output_format);
    }

    if (cmdline_opts->import) {
        if (!g_file_test (cmdline_opts->import_file, G_FILE_TEST_EXISTS) || !g_file_test (cmdline_opts->import_file, G_FILE_TEST_IS_REGULAR)) {
            g_printerr (_("%s doesn't exist or is not a valid file.\n"), cmdline_opts->import_file);
            return FALSE;
        }

        gchar *pwd = get_pwd (_("Type the password for the file you want to import: "), STDIN_FILENO);
        if (pwd == NULL) {
            return FALSE;
        }

        GSList *otps = get_data_from_provider (cmdline_opts->import_type, cmdline_opts->import_file, pwd, db_data->max_file_size_from_memlock, json_dumpb (db_data->in_memory_json_data, NULL, 0, 0), &err);
        if (otps == NULL) {
            const gchar *msg = "An error occurred while importing, so nothing has been added to the database.";
            gchar *msg_with_err = NULL;
            if (err != NULL) {
                msg_with_err = g_strconcat (msg, " The error is: ", err->message, NULL);
            }
            g_printerr ("%s\n", err == NULL ? msg : msg_with_err);
            if (err != NULL) {
                g_free (msg_with_err);
                g_clear_error (&err);
            }
            gcry_free (pwd);

            return FALSE;
        }
        gcry_free (pwd);

        add_otps_to_db (otps, db_data);
        free_otps_gslist (otps, g_slist_length (otps));

        update_db (db_data, &err);
        if (err != NULL && !g_error_matches (err, missing_file_gquark (), MISSING_FILE_ERRCODE)) {
            g_printerr ("Error while updating the database: %s\n", err->message);
            g_clear_error (&err);
            return FALSE;
        }
        g_clear_error (&err);
        reload_db (db_data, &err);
        if (err != NULL && !g_error_matches (err, missing_file_gquark (), MISSING_FILE_ERRCODE)) {
            g_printerr ("Error while reloading the database: %s\n", err->message);
            g_clear_error (&err);
            return FALSE;
        }
        g_clear_error (&err);
        g_print ("Data successfully imported.\n");
    }

    if (cmdline_opts->export) {
        gchar *export_directory;
#ifdef IS_FLATPAK
        export_directory = g_get_user_data_dir ();
#else
        export_directory = (cmdline_opts->export_dir != NULL) ? cmdline_opts->export_dir : (gchar *)g_get_home_dir ();
        if (!g_file_test (export_directory, G_FILE_TEST_IS_DIR)) {
            g_printerr (_("%s is not a directory or the folder doesn't exist. The output will be saved into the HOME directory.\n"), export_directory);
            export_directory = (gchar *)g_get_home_dir ();
        }
#endif
        gboolean exported = FALSE;
        gchar *export_pwd = NULL, *exported_file_path = NULL, *ret_msg = NULL;
        if (g_ascii_strcasecmp (cmdline_opts->export_type, FREEOTPPLUS_PLAIN_ACTION_NAME) == 0) {
            exported_file_path = g_build_filename (export_directory, "freeotpplus-exports.txt", NULL);
            ret_msg = export_freeotpplus (exported_file_path, db_data->in_memory_json_data);
            exported = TRUE;
        } else if (g_ascii_strcasecmp (cmdline_opts->export_type, AEGIS_PLAIN_ACTION_NAME) == 0 || g_ascii_strcasecmp (cmdline_opts->export_type, AEGIS_ENC_ACTION_NAME) == 0) {
            if (g_ascii_strcasecmp (cmdline_opts->export_type, AEGIS_ENC_ACTION_NAME) == 0) {
                export_pwd = get_pwd (_("Type the export encryption password: "), STDIN_FILENO);
                if (export_pwd == NULL) {
                    return FALSE;
                }
            }
            exported_file_path = g_build_filename (export_directory, export_pwd != NULL ? "aegis_exports.json.aes" : "aegis_exports.json", NULL);
            ret_msg = export_aegis (exported_file_path, export_pwd, db_data->in_memory_json_data);
            gcry_free (export_pwd);
            exported = TRUE;
        } else if (g_ascii_strcasecmp (cmdline_opts->export_type, TWOFAS_PLAIN_ACTION_NAME) == 0 || g_ascii_strcasecmp (cmdline_opts->export_type, TWOFAS_ENC_ACTION_NAME) == 0) {
            if (g_ascii_strcasecmp (cmdline_opts->export_type, TWOFAS_ENC_ACTION_NAME) == 0) {
                export_pwd = get_pwd (_("Type the export encryption password: "), STDIN_FILENO);
                if (export_pwd == NULL) {
                    return FALSE;
                }
            }
            exported_file_path = g_build_filename (export_directory, export_pwd != NULL ? "twofas_encrypted_v4.2fas" : "twofas_plain_v4.2fas", NULL);
            ret_msg = export_twofas (exported_file_path, export_pwd, db_data->in_memory_json_data);
            gcry_free (export_pwd);
            exported = TRUE;
        } else if (g_ascii_strcasecmp (cmdline_opts->export_type, AUTHPRO_PLAIN_ACTION_NAME) == 0 || g_ascii_strcasecmp (cmdline_opts->export_type, AUTHPRO_ENC_ACTION_NAME) == 0) {
            if (g_ascii_strcasecmp (cmdline_opts->export_type, AUTHPRO_ENC_ACTION_NAME) == 0) {
                export_pwd = get_pwd (_("Type the export encryption password: "), STDIN_FILENO);
                if (export_pwd == NULL) {
                    return FALSE;
                }
            }
            exported_file_path = g_build_filename (export_directory, export_pwd != NULL ? "authpro_encrypted.bin" : "authpro_plain.json", NULL);
            ret_msg = export_authpro (exported_file_path, export_pwd, db_data->in_memory_json_data);
            gcry_free (export_pwd);
            exported = TRUE;
        }
        if (ret_msg != NULL) {
            g_printerr (_("An error occurred while exporting the data: %s\n"), ret_msg);
            g_free (ret_msg);
        } else {
            if (exported) {
                g_print (_("Data successfully exported to: %s\n"), exported_file_path);
            } else {
                gchar *msg = g_strconcat ("Option not recognized: ", cmdline_opts->export_type, NULL);
                g_print ("%s\n", msg);
                g_free (msg);
                return FALSE;
            }
        }
        g_free (exported_file_path);
    }

    return TRUE;
}

static gchar *
resolve_db_path (const gchar *database_arg)
{
    /* If an explicit path or name was given on the command line */
    if (database_arg != NULL) {
        /* If it looks like a path (contains a separator), use it directly */
        if (g_path_is_absolute (database_arg) || strchr (database_arg, G_DIR_SEPARATOR) != NULL) {
            return g_strdup (database_arg);
        }
        /* Otherwise treat it as a database name and look it up */
        g_autoptr (GPtrArray) db_list = gsettings_common_get_db_list ();
        if (db_list != NULL) {
            for (guint i = 0; i < db_list->len; i++) {
                DbListEntry *entry = g_ptr_array_index (db_list, i);
                if (g_strcmp0 (entry->name, database_arg) == 0) {
                    return g_strdup (entry->path);
                }
            }
        }
        g_printerr (_("Database '%s' not found in the known database list.\n"), database_arg);
        return NULL;
    }

    /* No argument given — try GSettings / GKeyFile */
    gchar *db_path = gsettings_common_get_db_path ();
    if (db_path != NULL && db_path[0] != '\0') {
        if (!g_file_test (db_path, G_FILE_TEST_EXISTS)) {
            g_printerr ("Database file/location (%s) does not exist.\n", db_path);
            g_free (db_path);
            return NULL;
        }
        return db_path;
    }
    g_free (db_path);

    /* Last resort: ask interactively */
    g_print ("%s", _("Type the absolute path to the database: "));
    gchar *typed = g_malloc0 (MAX_ABS_PATH_LEN);
    if (fgets (typed, MAX_ABS_PATH_LEN, stdin) == NULL) {
        g_printerr ("%s\n", _("Couldn't get db path from stdin"));
        g_free (typed);
        return NULL;
    }
    char *nl = strchr (typed, '\n');
    if (nl) { *nl = '\0'; }
    if (!g_file_test (typed, G_FILE_TEST_EXISTS)) {
        g_printerr (_("File '%s' does not exist\n"), typed);
        g_free (typed);
        return NULL;
    }
    return typed;
}


static gchar *
get_pwd (const gchar *pwd_msg,
         int password_fd)
{
    const size_t BUFFER_SIZE = 512;
    gchar *pwd = gcry_calloc_secure (BUFFER_SIZE, 1);
    if (!pwd) return NULL;

    if (isatty (password_fd)) {
        g_print ("%s", pwd_msg);
        fflush (stdout);
    }

    struct termios old, new;
    gboolean term_fixed = FALSE;
    if (isatty (password_fd) && tcgetattr (password_fd, &old) == 0) {
        new = old;
        new.c_lflag &= ~(ECHO | ECHOE | ECHOK | ECHONL);
        if (tcsetattr (password_fd, TCSAFLUSH, &new) == 0) {
            term_fixed = TRUE;
        }
    }

    size_t len = 0;
    while (len < BUFFER_SIZE - 1) {
        ssize_t n = read (password_fd, &pwd[len], 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0 || pwd[len] == '\n') break;
        len++;
    }
    pwd[len] = '\0';

    if (term_fixed) {
        tcsetattr (password_fd, TCSAFLUSH, &old);
    }
    if (isatty (password_fd)) {
        g_print ("\n");
    }

    if (len == BUFFER_SIZE - 1) {
        g_printerr ("Warning: password was truncated at %zu bytes.\n", BUFFER_SIZE - 1);
    }

    if (len == 0) {
        g_printerr ("%s\n", _("Empty password not allowed"));
        gcry_free (pwd);
        return NULL;
    }

    return pwd;
}


