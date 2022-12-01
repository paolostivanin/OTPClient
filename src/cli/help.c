#include <glib.h>
#include <glib/gi18n.h>
#include "version.h"

static void print_main_help     (const gchar *prg_name);

static void print_show_help     (const gchar *prg_name);

static void print_export_help   (const gchar *prg_name);


gboolean show_help (const gchar *prg_name,
                    const gchar *help_command)
{
    gboolean help_displayed = FALSE;
    if (g_strcmp0 (help_command, "-h") == 0 || g_strcmp0 (help_command, "--help") == 0 || g_strcmp0 (help_command, "help") == 0) {
        print_main_help (prg_name);
        help_displayed = TRUE;
    } else if (g_strcmp0 (help_command, "-v") == 0 || g_strcmp0 (help_command, "--version") == 0) {
        g_print ("%s v%s\n", PROJECT_NAME, PROJECT_VER);
        help_displayed = TRUE;
    } else if (g_strcmp0 (help_command, "--help-show") == 0 || g_strcmp0 (help_command, "help-show") == 0) {
        print_show_help (prg_name);
        help_displayed = TRUE;
    } else if (g_strcmp0 (help_command, "--help-export") == 0 || g_strcmp0 (help_command, "help-export") == 0) {
        print_export_help (prg_name);
        help_displayed = TRUE;
    } else if (help_command == NULL || g_utf8_strlen (help_command, -1) < 2) {
        print_main_help (prg_name);
        help_displayed = TRUE;
    }

    return help_displayed;
}


static void
print_main_help (const gchar *prg_name)
{
    g_print (_("Usage:\n  %s <main option> [option 1] [option 2] ...\n"), prg_name);
    g_print ("\n");
    // Translators: please do not translate 'Help'
    g_print (_("Help Options:\n"));
    // Translators: please do not translate '-h, --help'
    g_print (_("  -h, --help\t\tShow this help\n"));
    // Translators: please do not translate '--help-show'
    g_print (_("  --help-show\t\tShow options\n"));
    // Translators: please do not translate '--help-export'
    g_print (_("  --help-export\t\tExport options\n"));
    g_print ("\n");
    g_print (_("Main Options:\n"));
    // Translators: please do not translate '-v, --version'
    g_print (_("  -v, --version\t\t\t\tShow program version\n"));
    // Translators: please do not translate 'show <-a ..> [-i ..] [-m] [-n]'
    g_print (_("  show <-a ..> [-i ..] [-m] [-n]\tShow a token\n"));
    // Translators: please do not translate 'list'
    g_print (_("  list\t\t\t\t\tList all pairs of account and issuer\n"));
    // Translators: please do not translate 'export <-t ..> [-d ..]'
    g_print (_("  export <-t ..> [-d ..]\t\tExport data\n"));
    g_print ("\n");
}


static void
print_show_help (const gchar *prg_name)
{
    // Translators: please do not translate '%s show'
    g_print (_("Usage:\n  %s show <-a ..> [-i ..] [-m]\n"), prg_name);
    g_print ("\n");
    // Translators: please do not translate 'Show'
    g_print (_("Show Options:\n"));
    // Translators: please do not translate '-a, --account'
    g_print (_("  -a, --account\t\tThe account name (mandatory)\n"));
    // Translators: please do not translate '-i, --issuer'
    g_print (_("  -i, --issuer\t\tThe issuer name (optional)\n"));
    // Translators: please do not translate '-m, --match-exactly'
    g_print (_("  -m, --match-exactly\tShow the token only if it matches exactly the account and/or the issuer (optional)\n"));
    // Translators: please do not translate '-n, --next'
    g_print (_("  -n, --next\tShow also the next token, not only the current one (optional, valid only for TOTP)\n"));
    g_print ("\n");
}


static void
print_export_help (const gchar *prg_name)
{
    // Translators: please do not translate 'export'
    g_print (_("Usage:\n  %s export <-t> <andotp | freeotpplus | aegis> [-d ..]\n"), prg_name);
    g_print ("\n");
    // Translators: please do not translate 'Export'
    g_print (_("Export Options:\n"));
    // Translators: please do not translate '-t, --type'
    g_print (_("  -t, --type\t\tExport format. Must be either one of: andotp_plain, andotp_encrypted, freeotpplus, aegis\n"));
    // Translators: please do not translate '-d, --directory'
    g_print (_("  -d, --directory\tThe output directory where the exported file will be saved.\n"));
    g_print (_("\t\t\tIf nothing is specified OR flatpak is being used, the output folder will be the user's HOME directory.\n"));
    g_print ("\n");
}
