#include <glib.h>
#include <gio/gio.h>
#include <gcrypt.h>
#include <glib/gi18n.h>
#include "../common/file-size.h"
#include "version.h"
#include "../common/import-export.h"
#include "main.h"

static gint      handle_local_options  (GApplication            *application,
                                        GVariantDict            *options,
                                        gpointer                 user_data);

static int       command_line          (GApplication            *application,
                                        GApplicationCommandLine *cmdline,
                                        gpointer                 user_data);

static gboolean  parse_options         (GApplicationCommandLine *cmdline,
                                        CmdlineOpts             *cmdline_opts);

static gboolean  is_valid_type         (const gchar             *type);

static gchar    *format_supported_types(void);

static void      print_supported_types (GApplicationCommandLine *cmdline);

static void      g_free_cmdline_opts   (CmdlineOpts             *co);

static const gchar *supported_types[] = {AEGIS_PLAIN_ACTION_NAME, AEGIS_ENC_ACTION_NAME,
                                         TWOFAS_PLAIN_ACTION_NAME, TWOFAS_ENC_ACTION_NAME,
                                         AUTHPRO_PLAIN_ACTION_NAME, AUTHPRO_ENC_ACTION_NAME,
                                         FREEOTPPLUS_PLAIN_ACTION_NAME,
                                         NULL};


gint
main (gint    argc,
      gchar **argv)
{
    g_autofree gchar *supported_types_str = format_supported_types ();
    g_autofree gchar *type_msg = g_strconcat ("The import/export type for the database (to be used with --import/--export, mandatory). Must be either one of: ",
                                              supported_types_str,
                                              NULL);

    GOptionEntry entries[] =
            {
                    { "database", 'd', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, NULL, "(optional) path to the database. Default value is taken from otpclient.cfg", NULL },
                    { "show", 's', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, "Show a token for a given account.", NULL },
                    { "account", 'a', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, NULL, "Account name (to be used with --show, mandatory)", NULL},
                    { "issuer", 'i', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, NULL, "Issuer (to be used with --show, optional)", NULL},
                    { "match-exact", 'm', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, "Match exactly the provided account/issuer (to be used with --show, optional)", NULL},
                    { "show-next", 'n', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, "Show also next OTP (to be used with --show, optional)", NULL},
                    { "list", 'l', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, "List all accounts and issuers for a given database.", NULL },
                    { "list-types", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, "List supported import/export types.", NULL },
                    { "import", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, "Import a database.", NULL },
                    { "export", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, "Export a database.", NULL },
                    { "type", 't', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, NULL, type_msg, NULL },
                    { "file", 'f', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, NULL, "File to import (to be used with --import, mandatory).", NULL },
#ifndef IS_FLATPAK
                    { "output-dir", 'o', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, NULL, "The output directory (defaults to the user's home. To be used with --export, optional)", NULL },
#endif
                    { "password-file", 'p', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, NULL, "(optional) Read database password from a file instead of stdin.", NULL },
                    { "version", 'v', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, "Show the program version.", NULL },
                    { NULL }
            };


    const gchar *ctx_text = "- Highly secure and easy to use OTP client that supports both TOTP and HOTP";

    GApplication *app = g_application_new ("com.github.paolostivanin.OTPClient", G_APPLICATION_HANDLES_COMMAND_LINE);

    g_application_add_main_option_entries (app, entries);
    g_application_set_option_context_parameter_string (app, ctx_text);

    g_signal_connect (app, "handle-local-options", G_CALLBACK(handle_local_options), NULL);
    g_signal_connect (app, "command-line", G_CALLBACK(command_line), NULL);

    int status = g_application_run (app, argc, argv);

    g_object_unref (app);

    return status;
}


void
free_dbdata (DatabaseData *db_data)
{
    gcry_free (db_data->key);
    g_free (db_data->db_path);
    g_slist_free_full (db_data->objects_hash, g_free);
    json_decref (db_data->in_memory_json_data);
    g_free (db_data);
}


static gint
handle_local_options (GApplication      *application __attribute__((unused)),
                      GVariantDict      *options,
                      gpointer           user_data   __attribute__((unused)))
{
    guint32 count;
    if (g_variant_dict_lookup (options, "version", "b", &count)) {
        gchar *msg = g_strconcat ("OTPClient version ", PROJECT_VER, NULL);
        g_print ("%s\n", msg);
        g_free (msg);
        return 0;
    }
    return -1;
}


static int
command_line (GApplication                *application __attribute__((unused)),
              GApplicationCommandLine     *cmdline,
              gpointer                     user_data   __attribute__((unused)))
{
    CmdlineOpts *cmdline_opts = g_new0 (CmdlineOpts, 1);
    cmdline_opts->database = NULL;
    cmdline_opts->show = FALSE;
    cmdline_opts->account = NULL;
    cmdline_opts->issuer = NULL;
    cmdline_opts->match_exact = FALSE;
    cmdline_opts->show_next = FALSE;
    cmdline_opts->list = FALSE;
    cmdline_opts->list_types = FALSE;
    cmdline_opts->import = FALSE;
    cmdline_opts->import_type = NULL;
    cmdline_opts->import_file = NULL;
    cmdline_opts->export = FALSE;
    cmdline_opts->export_type = NULL;
    cmdline_opts->export_dir = NULL;
    cmdline_opts->password_file = NULL;

    if (!parse_options (cmdline, cmdline_opts)) {
        g_free_cmdline_opts (cmdline_opts);
        return -1;
    }

    if (cmdline_opts->list_types) {
        print_supported_types (cmdline);
        g_free_cmdline_opts (cmdline_opts);
        return 0;
    }

    DatabaseData *db_data = g_new0 (DatabaseData, 1);
    db_data->key_stored = FALSE;
    db_data->objects_hash = NULL;
    db_data->max_file_size_from_memlock = 0;

    gint32 memlock_ret_value = set_memlock_value (&db_data->max_file_size_from_memlock);
    if (memlock_ret_value == MEMLOCK_ERR) {
        g_printerr (_("Couldn't get the memlock value, therefore secure memory cannot be allocated. Please have a look at the following page before re-running OTPClient:"
                    "https://github.com/paolostivanin/OTPClient/wiki/Secure-Memory-Limitations"));
        g_free (db_data);
        g_free_cmdline_opts (cmdline_opts);
        return -1;
    }

    gchar *init_msg = init_libs (db_data->max_file_size_from_memlock);
    if (init_msg != NULL) {
        g_application_command_line_printerr (cmdline, "Error while initializing GCrypt: %s\n", init_msg);
        g_free (init_msg);
        g_free (db_data);
        g_free_cmdline_opts (cmdline_opts);
        return -1;
    }

    if (!exec_action (cmdline_opts, db_data)) {
        free_dbdata (db_data);
        g_free_cmdline_opts (cmdline_opts);
        return -1;
    }

    free_dbdata (db_data);
    g_free_cmdline_opts (cmdline_opts);

    return 0;
}


static gboolean
parse_options (GApplicationCommandLine *cmdline,
               CmdlineOpts             *cmdline_opts)
{
    GVariantDict *options = g_application_command_line_get_options_dict (cmdline);

    g_variant_dict_lookup (options, "database", "s", &cmdline_opts->database);
    g_variant_dict_lookup (options, "list-types", "b", &cmdline_opts->list_types);
    g_variant_dict_lookup (options, "password-file", "s", &cmdline_opts->password_file);

    g_variant_dict_lookup (options, "show", "b", &cmdline_opts->show);
    g_variant_dict_lookup (options, "list", "b", &cmdline_opts->list);
    g_variant_dict_lookup (options, "import", "b", &cmdline_opts->import);
    g_variant_dict_lookup (options, "export", "b", &cmdline_opts->export);

    if (cmdline_opts->list_types + cmdline_opts->show + cmdline_opts->list + cmdline_opts->import + cmdline_opts->export == 0) {
        g_application_command_line_print (cmdline, "Please provide one action (--show, --list, --import, --export, or --list-types).\n");
        return FALSE;
    }

    if (cmdline_opts->list_types + cmdline_opts->show + cmdline_opts->list + cmdline_opts->import + cmdline_opts->export > 1) {
        g_application_command_line_print (cmdline, "Please provide only one action at a time (--show, --list, --import, --export, or --list-types).\n");
        return FALSE;
    }

    if (cmdline_opts->show) {
        g_variant_dict_lookup (options, "account", "s", &cmdline_opts->account);
        g_variant_dict_lookup (options, "issuer", "s", &cmdline_opts->issuer);
        if (cmdline_opts->account == NULL && cmdline_opts->issuer == NULL) {
            g_application_command_line_print (cmdline, "Please provide at least the account or issuer option.\n");
            return FALSE;
        }
        g_variant_dict_lookup (options, "match-exact", "b", &cmdline_opts->match_exact);
        g_variant_dict_lookup (options, "show-next", "b", &cmdline_opts->show_next);
    } else {
        if (g_variant_dict_lookup (options, "account", "s", &cmdline_opts->account) ||
            g_variant_dict_lookup (options, "issuer", "s", &cmdline_opts->issuer) ||
            g_variant_dict_lookup (options, "match-exact", "b", &cmdline_opts->match_exact) ||
            g_variant_dict_lookup (options, "show-next", "b", &cmdline_opts->show_next)) {
            g_application_command_line_print (cmdline, "The account/issuer filters and matching options can only be used with --show.\n");
            return FALSE;
        }
    }

    if (cmdline_opts->import) {
        if (!g_variant_dict_lookup (options, "type", "s", &cmdline_opts->import_type)) {
            g_application_command_line_print (cmdline, "Please provide an import type.\n");
            return FALSE;
        }
        if (!is_valid_type (cmdline_opts->import_type)) {
            g_application_command_line_print (cmdline, "Please provide a valid import type (see --help).\n");
            return FALSE;
        }
        if (!g_variant_dict_lookup (options, "file", "s", &cmdline_opts->import_file)) {
            g_application_command_line_print (cmdline, "Please provide a file to import.\n");
            return FALSE;
        }
    }

    if (cmdline_opts->export) {
        if (!g_variant_dict_lookup (options, "type", "s", &cmdline_opts->export_type)) {
            g_application_command_line_print (cmdline, "Please provide an export type (see --help).\n");
            return FALSE;
        }
        if (!is_valid_type (cmdline_opts->export_type)) {
            g_application_command_line_print (cmdline, "Please provide a valid export type.\n");
            return FALSE;
        }
#ifndef IS_FLATPAK
        g_variant_dict_lookup (options, "output-dir", "s", &cmdline_opts->export_dir);
#endif
    }

    if (!cmdline_opts->import && !cmdline_opts->export) {
        gchar *unused_type = NULL;
        if (g_variant_dict_lookup (options, "type", "s", &unused_type)) {
            g_application_command_line_print (cmdline, "The --type option can only be used with --import or --export.\n");
            g_free (unused_type);
            return FALSE;
        }
        if (g_variant_dict_lookup (options, "file", "s", &cmdline_opts->import_file)) {
            g_application_command_line_print (cmdline, "The --file option can only be used with --import.\n");
            return FALSE;
        }
#ifndef IS_FLATPAK
        if (g_variant_dict_lookup (options, "output-dir", "s", &cmdline_opts->export_dir)) {
            g_application_command_line_print (cmdline, "The --output-dir option can only be used with --export.\n");
            return FALSE;
        }
#endif
    }
    return TRUE;
}


static gboolean
is_valid_type (const gchar *type)
{
    for (gint i = 0; supported_types[i] != NULL; i++) {
        if (g_strcmp0 (type, supported_types[i]) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static gchar *
format_supported_types (void)
{
    return g_strjoinv (", ", (gchar **)supported_types);
}


static void
print_supported_types (GApplicationCommandLine *cmdline)
{
    g_autofree gchar *supported_types_str = format_supported_types ();
    g_application_command_line_print (cmdline, "Supported types: %s\n", supported_types_str);
}


static void
g_free_cmdline_opts (CmdlineOpts *co)
{
    g_free (co->database);
    g_free (co->account);
    g_free (co->issuer);
    g_free (co->import_type);
    g_free (co->import_file);
    g_free (co->export_type);
    g_free (co->export_dir);
    g_free (co->password_file);
    g_free (co);
}
