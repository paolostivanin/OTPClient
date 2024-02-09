#include <glib.h>
#include <gio/gio.h>
#include "version.h"
#include "main.h"
#include "../common/common.h"

static gint      handle_local_options  (GApplication            *application,
                                        GVariantDict            *options,
                                        gpointer                 user_data);

static int       command_line          (GApplication            *application,
                                        GApplicationCommandLine *cmdline,
                                        gpointer                 user_data);

static gboolean  parse_options         (GApplicationCommandLine *cmdline,
                                        CmdlineOpts             *cmdline_opts);

static void      g_free_cmdline_opts   (CmdlineOpts             *co);


gint
main (gint    argc,
      gchar **argv)
{
    GOptionEntry entries[] =
            {
#ifndef USE_FLATPAK_APP_FOLDER
                    { "database", 'd', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, NULL, "(optional) path to the database. Default value is taken from otpclient.cfg", NULL },
#endif
                    { "show", 's', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, "Show a token for a given account.", NULL },
                    { "account", 'a', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, NULL, "Account name (to be used with --show, mandatory)", NULL},
                    { "issuer", 'i', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, NULL, "Issuer (to be used with --show, optional)", NULL},
                    { "match-exact", 'm', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, "Match exactly the provided account/issuer (to be used with --show, optional)", NULL},
                    { "show-next", 'n', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, "Show also next OTP (to be used with --show, optional)", NULL},
                    { "list", 'l', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, "List all accounts and issuers for a given database.", NULL },
                    { "export", 'e', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, "Export a database.", NULL },
                    { "type", 't', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, NULL, "The export type for the database. Must be either one of: andotp_plain, andotp_encrypted, freeotpplus, aegis, aegis_encrypted (to be used with --export, mandatory)", NULL },
#ifndef USE_FLATPAK_APP_FOLDER
                    { "output-dir", 'o', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, NULL, "The output directory (defaults to the user's home. To be used with --export, optional)", NULL },
#endif
                    { "version", 'v', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, "Show the program version.", NULL },
                    { NULL }
            };


    const gchar *ctx_text = "- Highly secure and easy to use OTP client that supports both TOTP and HOTP";

    GApplication *app = g_application_new ("com.github.paolostivanin.OTPClient", G_APPLICATION_HANDLES_COMMAND_LINE);

    g_application_add_main_option_entries (app, entries);
    g_application_set_option_context_parameter_string (app, ctx_text);

    g_signal_connect (app, "handle-local-options", G_CALLBACK (handle_local_options), NULL);
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
    json_decref (db_data->json_data);
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
    DatabaseData *db_data = g_new0 (DatabaseData, 1);
    db_data->key_stored = FALSE;
    db_data->objects_hash = NULL;

    db_data->max_file_size_from_memlock = get_max_file_size_from_memlock ();
    gchar *init_msg = init_libs (db_data->max_file_size_from_memlock);
    if (init_msg != NULL) {
        g_application_command_line_printerr(cmdline, "Error while initializing GCrypt: %s\n", init_msg);
        g_free (init_msg);
        g_free (db_data);
        return -1;
    }

    CmdlineOpts *cmdline_opts = g_new0 (CmdlineOpts, 1);
    cmdline_opts->database = NULL;
    cmdline_opts->show = FALSE;
    cmdline_opts->account = NULL;
    cmdline_opts->issuer = NULL;
    cmdline_opts->match_exact = FALSE;
    cmdline_opts->show_next = FALSE;
    cmdline_opts->list = FALSE;
    cmdline_opts->export = FALSE;
    cmdline_opts->export_type = NULL;
    cmdline_opts->export_dir = NULL;

    if (!parse_options (cmdline, cmdline_opts)) {
        g_free (db_data);
        g_free_cmdline_opts (cmdline_opts);
        return -1;
    }

    if (!exec_action (cmdline_opts, db_data)) {
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

    if (g_variant_dict_lookup (options, "show", "b", &cmdline_opts->show)) {
        if (!g_variant_dict_lookup (options, "account", "s", &cmdline_opts->account)) {
            g_application_command_line_print (cmdline, "Please provide at least the account option.\n");
            return FALSE;
        }
        g_variant_dict_lookup (options, "issuer", "s", &cmdline_opts->issuer);
        g_variant_dict_lookup (options, "match-exact", "b", &cmdline_opts->match_exact);
        g_variant_dict_lookup (options, "show-next", "b", &cmdline_opts->show_next);
    }

    g_variant_dict_lookup (options, "list", "b", &cmdline_opts->list);

    if (g_variant_dict_lookup (options, "export", "b", &cmdline_opts->export)) {
        if (!g_variant_dict_lookup (options, "type", "s", &cmdline_opts->export_type)) {
            g_application_command_line_print (cmdline, "Please provide at least export type.\n");
            return FALSE;
        }
#ifndef USE_FLATPAK_APP_FOLDER
        g_variant_dict_lookup (options, "output-dir", "s", &cmdline_opts->export_dir);
#endif
    }
    return TRUE;
}


static void
g_free_cmdline_opts (CmdlineOpts *co)
{
    g_free (co->database);
    g_free (co->account);
    g_free (co->issuer);
    g_free (co->export_type);
    g_free (co->export_dir);
    g_free (co);
}
