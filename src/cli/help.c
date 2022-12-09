#include <glib.h>
#include <glib/gi18n.h>
#include "version.h"
#include "../common/common.h"

static void print_main_help   (const gchar *prg_name);

static void print_show_help   (const gchar *prg_name);

static void print_export_help (const gchar *prg_name);


gboolean show_help (const gchar *prg_name,
                    const gchar *help_command)
{
    gboolean help_displayed = FALSE;
    if (g_strcmp0 (help_command, "-h") == 0 || g_strcmp0 (help_command, "--help") == 0 || g_strcmp0 (help_command, "help") == 0 ||
            help_command == NULL || g_utf8_strlen (help_command, -1) < 2) {
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
    }

    return help_displayed;
}


static void
print_main_help (const gchar *prg_name)
{
    GString *msg = g_string_new (_("Usage:\n  %s <main option> [option 1] [option 2] ..."));
#if GLIB_CHECK_VERSION(2, 68, 0)
    g_string_replace (msg, "%s", prg_name, 0);
#else
    g_string_replace_backported (msg, "%s", prg_name, 0);
#endif
    g_print ("%s\n\n", msg->str);
    g_string_free (msg, TRUE);

    // Translators: please do not translate 'help'
    g_print ("%s\n", _("help command options:"));

    // Translators: please do not translate '-h, --help'
    g_print ("%s\n", _("  -h, --help\t\tShow this help"));

    // Translators: please do not translate '--help-show'
    g_print ("%s\n", _("  --help-show\t\tShow options"));

    // Translators: please do not translate '--help-export'
    g_print ("%s\n\n", _("  --help-export\t\tExport options"));
    g_print ("%s\n", _("Main options:"));

    // Translators: please do not translate '-v, --version'
    g_print ("%s\n", _("  -v, --version\t\t\t\tShow program version"));

    // Translators: please do not translate 'show <-a ..> [-i ..] [-m] [-n]'
    g_print ("%s\n", _("  show <-a ..> [-i ..] [-m] [-n]\tShow a token"));

    // Translators: please do not translate 'list'
    g_print ("%s\n", _("  list\t\t\t\t\tList all pairs of account and issuer"));

    // Translators: please do not translate 'export <-t ..> [-d ..]'
    g_print ("%s\n\n", _("  export <-t ..> [-d ..]\t\tExport data"));
}


static void
print_show_help (const gchar *prg_name)
{
    // Translators: please do not translate '%s show'
    GString *msg = g_string_new (_("Usage:\n  %s show <-a ..> [-i ..] [-m]"));
#if GLIB_CHECK_VERSION(2, 68, 0)
    g_string_replace (msg, "%s", prg_name, 0);
#else
    g_string_replace_backported (msg, "%s", prg_name, 0);
#endif
    g_print ("%s\n\n", msg->str);
    g_string_free (msg, TRUE);

    // Translators: please do not translate 'show'
    g_print ("%s\n", _("show command options:"));
    // Translators: please do not translate '-a, --account'
    g_print ("%s\n", _("  -a, --account\t\tThe account name (mandatory)"));
    // Translators: please do not translate '-i, --issuer'
    g_print ("%s\n", _("  -i, --issuer\t\tThe issuer name (optional)"));
    // Translators: please do not translate '-m, --match-exactly'
    g_print ("%s\n", _("  -m, --match-exactly\tShow the token only if it matches exactly the account and/or the issuer (optional)"));
    // Translators: please do not translate '-n, --next'
    g_print ("%s\n\n", _("  -n, --next\tShow also the next token, not only the current one (optional, valid only for TOTP)"));
}


static void
print_export_help (const gchar *prg_name)
{
    // Translators: please do not translate '%s export'
    GString *msg = g_string_new (_("Usage:\n  %s export <-t> <andotp | freeotpplus | aegis> [-d ..]"));
#if GLIB_CHECK_VERSION(2, 68, 0)
    g_string_replace (msg, "%s", prg_name, 0);
#else
    g_string_replace_backported (msg, "%s", prg_name, 0);
#endif
    g_print ("%s\n\n", msg->str);
    g_string_free (msg, TRUE);

    // Translators: please do not translate 'export'
    g_print ("%s\n", _("export command options:"));
    // Translators: please do not translate '-t, --type'
    g_print ("%s\n", _("  -t, --type\t\tExport format. Must be either one of: andotp_plain, andotp_encrypted, freeotpplus, aegis"));
    // Translators: please do not translate '-d, --directory'
    g_print ("%s\n", _("  -d, --directory\tThe output directory where the exported file will be saved."));
    g_print ("%s\n\n", _("\t\t\tIf nothing is specified OR flatpak is being used, the output folder will be the user's HOME directory."));
}
