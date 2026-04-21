#include <glib.h>
#include <gio/gio.h>
#include <gcrypt.h>
#include <glib/gi18n.h>
#include "../common/file-size.h"
#include "version.h"
#include "../common/import-export.h"
#include "../common/settings-import-export.h"
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
    bindtextdomain (GETTEXT_PACKAGE, LOCALE_DIR);
    bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
    textdomain (GETTEXT_PACKAGE);

    g_autofree gchar *supported_types_str = format_supported_types ();
    g_autofree gchar *type_msg = g_strconcat (_("The import/export type for the database (to be used with --import/--export, mandatory). Must be either one of: "),
                                              supported_types_str,
                                              NULL);

    GOptionEntry entries[] =
            {
                    { "database", 'd', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, NULL, _("(optional) path to the database or name from --list-databases. Default value is taken from GSettings/otpclient.cfg"), NULL },
                    { "show", 's', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, _("Show a token for a given account."), NULL },
                    { "account", 'a', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, NULL, _("Account name (to be used with --show, mandatory)"), NULL},
                    { "issuer", 'i', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, NULL, _("Issuer (to be used with --show, optional)"), NULL},
                    { "match-exact", 'm', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, _("Match exactly the provided account/issuer (to be used with --show, optional)"), NULL},
                    { "show-next", 'n', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, _("Show also next OTP (to be used with --show, optional)"), NULL},
                    { "list", 'l', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, _("List all accounts and issuers for a given database."), NULL },
                    { "list-databases", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, _("List all known databases from the configuration."), NULL },
                    { "list-types", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, _("List supported import/export types."), NULL },
                    { "import", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, _("Import a database."), NULL },
                    { "export", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, _("Export a database."), NULL },
                    { "type", 't', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, NULL, type_msg, NULL },
                    { "file", 'f', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, NULL, _("File to import (to be used with --import, mandatory)."), NULL },
#ifndef IS_FLATPAK
                    { "output-dir", 'o', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, NULL, _("The output directory (defaults to the user's home. To be used with --export, optional)"), NULL },
#endif
                    { "password-file", 'p', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, NULL, _("(optional) Read database password from a file instead of stdin."), NULL },
                    { "export-settings", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, _("Export application settings as JSON."), NULL },
                    { "import-settings", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, _("Import application settings from a JSON file (requires --file)."), NULL },
                    { "output", 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, NULL, _("Output format for --show, --list, --list-databases: table (default), json, csv."), "FORMAT" },
                    { "version", 'v', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, _("Show the program version."), NULL },
                    { NULL }
            };


    const gchar *ctx_text = _("- Highly secure and easy to use OTP client that supports both TOTP and HOTP");

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
    db_invalidate_kdf_cache (db_data);
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
    cmdline_opts->list_databases = FALSE;
    cmdline_opts->list_types = FALSE;
    cmdline_opts->import = FALSE;
    cmdline_opts->import_type = NULL;
    cmdline_opts->import_file = NULL;
    cmdline_opts->export = FALSE;
    cmdline_opts->export_type = NULL;
    cmdline_opts->export_dir = NULL;
    cmdline_opts->password_file = NULL;
    cmdline_opts->export_settings = FALSE;
    cmdline_opts->import_settings = FALSE;
    cmdline_opts->output = NULL;
    cmdline_opts->output_format = OUTPUT_FORMAT_TABLE;

    if (!parse_options (cmdline, cmdline_opts)) {
        g_free_cmdline_opts (cmdline_opts);
        return -1;
    }

    if (cmdline_opts->list_types) {
        print_supported_types (cmdline);
        g_free_cmdline_opts (cmdline_opts);
        return 0;
    }

    if (cmdline_opts->list_databases) {
        /* list-databases doesn't need a DB loaded, just config access */
        if (!exec_action (cmdline_opts, NULL)) {
            g_free_cmdline_opts (cmdline_opts);
            return -1;
        }
        g_free_cmdline_opts (cmdline_opts);
        return 0;
    }

    if (cmdline_opts->export_settings) {
        GError *err = NULL;
        gchar *json = export_settings_to_json (&err);
        if (json == NULL) {
            g_application_command_line_printerr (cmdline, _("Error: %s\n"), err->message);
            g_clear_error (&err);
            g_free_cmdline_opts (cmdline_opts);
            return -1;
        }
        if (cmdline_opts->import_file != NULL) {
            if (!g_file_set_contents (cmdline_opts->import_file, json, -1, &err)) {
                g_application_command_line_printerr (cmdline, _("Error writing file: %s\n"), err->message);
                g_clear_error (&err);
                gcry_free (json);
                g_free_cmdline_opts (cmdline_opts);
                return -1;
            }
            g_application_command_line_print (cmdline, _("Settings exported to: %s\n"), cmdline_opts->import_file);
        } else {
            g_application_command_line_print (cmdline, "%s\n", json);
        }
        gcry_free (json);
        g_free_cmdline_opts (cmdline_opts);
        return 0;
    }

    if (cmdline_opts->import_settings) {
        GError *err = NULL;
        gchar *contents = NULL;
        if (!g_file_get_contents (cmdline_opts->import_file, &contents, NULL, &err)) {
            g_application_command_line_printerr (cmdline, _("Error reading file: %s\n"), err->message);
            g_clear_error (&err);
            g_free_cmdline_opts (cmdline_opts);
            return -1;
        }
        if (!import_settings_from_json (contents, &err)) {
            g_application_command_line_printerr (cmdline, _("Error: %s\n"), err->message);
            g_clear_error (&err);
            g_free (contents);
            g_free_cmdline_opts (cmdline_opts);
            return -1;
        }
        g_free (contents);
        g_application_command_line_print (cmdline, "%s", _("Settings imported successfully.\n"));
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
        g_application_command_line_printerr (cmdline, _("Error while initializing GCrypt: %s\n"), init_msg);
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
    g_variant_dict_lookup (options, "list-databases", "b", &cmdline_opts->list_databases);
    g_variant_dict_lookup (options, "import", "b", &cmdline_opts->import);
    g_variant_dict_lookup (options, "export", "b", &cmdline_opts->export);
    g_variant_dict_lookup (options, "export-settings", "b", &cmdline_opts->export_settings);
    g_variant_dict_lookup (options, "import-settings", "b", &cmdline_opts->import_settings);

    if (g_variant_dict_lookup (options, "output", "s", &cmdline_opts->output)) {
        if (g_ascii_strcasecmp (cmdline_opts->output, "table") == 0) {
            cmdline_opts->output_format = OUTPUT_FORMAT_TABLE;
        } else if (g_ascii_strcasecmp (cmdline_opts->output, "json") == 0) {
            cmdline_opts->output_format = OUTPUT_FORMAT_JSON;
        } else if (g_ascii_strcasecmp (cmdline_opts->output, "csv") == 0) {
            cmdline_opts->output_format = OUTPUT_FORMAT_CSV;
        } else {
            g_application_command_line_print (cmdline, _("Unknown --output value '%s'. Expected: table, json, csv.\n"), cmdline_opts->output);
            return FALSE;
        }
        if (!cmdline_opts->show && !cmdline_opts->list && !cmdline_opts->list_databases) {
            g_application_command_line_print (cmdline, "%s", _("The --output option only applies to --show, --list, or --list-databases.\n"));
            return FALSE;
        }
    }

    guint action_count = cmdline_opts->list_types + cmdline_opts->show + cmdline_opts->list + cmdline_opts->list_databases + cmdline_opts->import + cmdline_opts->export + cmdline_opts->export_settings + cmdline_opts->import_settings;
    if (action_count == 0) {
        g_application_command_line_print (cmdline, "%s", _("Please provide one action (--show, --list, --list-databases, --import, --export, --export-settings, --import-settings, or --list-types).\n"));
        return FALSE;
    }

    if (action_count > 1) {
        g_application_command_line_print (cmdline, "%s", _("Please provide only one action at a time.\n"));
        return FALSE;
    }

    if (cmdline_opts->show) {
        g_variant_dict_lookup (options, "account", "s", &cmdline_opts->account);
        g_variant_dict_lookup (options, "issuer", "s", &cmdline_opts->issuer);
        if (cmdline_opts->account == NULL && cmdline_opts->issuer == NULL) {
            g_application_command_line_print (cmdline, "%s", _("Please provide at least the account or issuer option.\n"));
            return FALSE;
        }
        g_variant_dict_lookup (options, "match-exact", "b", &cmdline_opts->match_exact);
        g_variant_dict_lookup (options, "show-next", "b", &cmdline_opts->show_next);
    } else {
        if (g_variant_dict_lookup (options, "account", "s", &cmdline_opts->account) ||
            g_variant_dict_lookup (options, "issuer", "s", &cmdline_opts->issuer) ||
            g_variant_dict_lookup (options, "match-exact", "b", &cmdline_opts->match_exact) ||
            g_variant_dict_lookup (options, "show-next", "b", &cmdline_opts->show_next)) {
            g_application_command_line_print (cmdline, "%s", _("The account/issuer filters and matching options can only be used with --show.\n"));
            return FALSE;
        }
    }

    if (cmdline_opts->import_settings) {
        if (!g_variant_dict_lookup (options, "file", "s", &cmdline_opts->import_file)) {
            g_application_command_line_print (cmdline, "%s", _("Please provide a file to import settings from (--file).\n"));
            return FALSE;
        }
    }

    if (cmdline_opts->export_settings) {
        /* --file is optional for export-settings; if given, write to file instead of stdout */
        g_variant_dict_lookup (options, "file", "s", &cmdline_opts->import_file);
    }

    if (cmdline_opts->import) {
        if (!g_variant_dict_lookup (options, "type", "s", &cmdline_opts->import_type)) {
            g_application_command_line_print (cmdline, "%s", _("Please provide an import type.\n"));
            return FALSE;
        }
        if (!is_valid_type (cmdline_opts->import_type)) {
            g_application_command_line_print (cmdline, "%s", _("Please provide a valid import type (see --help).\n"));
            return FALSE;
        }
        if (!g_variant_dict_lookup (options, "file", "s", &cmdline_opts->import_file)) {
            g_application_command_line_print (cmdline, "%s", _("Please provide a file to import.\n"));
            return FALSE;
        }
    }

    if (cmdline_opts->export) {
        if (!g_variant_dict_lookup (options, "type", "s", &cmdline_opts->export_type)) {
            g_application_command_line_print (cmdline, "%s", _("Please provide an export type (see --help).\n"));
            return FALSE;
        }
        if (!is_valid_type (cmdline_opts->export_type)) {
            g_application_command_line_print (cmdline, "%s", _("Please provide a valid export type.\n"));
            return FALSE;
        }
#ifndef IS_FLATPAK
        g_variant_dict_lookup (options, "output-dir", "s", &cmdline_opts->export_dir);
#endif
    }

    if (!cmdline_opts->import && !cmdline_opts->export) {
        gchar *unused_type = NULL;
        if (g_variant_dict_lookup (options, "type", "s", &unused_type)) {
            g_application_command_line_print (cmdline, "%s", _("The --type option can only be used with --import or --export.\n"));
            g_free (unused_type);
            return FALSE;
        }
        if (!cmdline_opts->import_settings && !cmdline_opts->export_settings &&
            g_variant_dict_lookup (options, "file", "s", &cmdline_opts->import_file)) {
            g_application_command_line_print (cmdline, "%s", _("The --file option can only be used with --import, --import-settings, or --export-settings.\n"));
            return FALSE;
        }
#ifndef IS_FLATPAK
        if (g_variant_dict_lookup (options, "output-dir", "s", &cmdline_opts->export_dir)) {
            g_application_command_line_print (cmdline, "%s", _("The --output-dir option can only be used with --export.\n"));
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
    g_autofree gchar *types_str = format_supported_types ();
    g_application_command_line_print (cmdline, _("Supported types: %s\n"), types_str);
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
    g_free (co->output);
    g_free (co);
}
