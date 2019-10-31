#include <glib.h>

static void print_main_help         (const gchar *prg_name);

static void print_show_help         (const gchar *prg_name);


gint show_help (const gchar *prg_name,
                const gchar *help_command)
{
    if (g_strcmp0 (help_command, "-h") == 0 || g_strcmp0 (help_command, "--help") == 0 ) {
        print_main_help (prg_name);
    } else if (g_strcmp0 (help_command, "--help-show") == 0) {
        print_show_help (prg_name);
    } else {
        print_main_help (prg_name);
    }

    return 0;
}


static void
print_main_help (const gchar *prg_name)
{
    g_print ("Usage:\n  %s <main option> [option 1], [option 2], ...\n", prg_name);
    g_print ("\n");
    g_print ("Help Options:\n");
    g_print ("  -h, --help\t\tShow this help\n");
    g_print ("  --help-show\t\tShow options\n");
    g_print ("\n");
    g_print ("Main Options:\n");
    g_print ("  -v, --version\t\tShow program version\n");
    g_print ("  --show\t\tShow a token\n");
    g_print ("  --list\t\tList all pairs of account and issuer\n");
    g_print ("\n");
}


static void
print_show_help (const gchar *prg_name)
{
    g_print ("Usage:\n  %s --show <-a ..> [-i ..] [-m]\n", prg_name);
    g_print ("\n");
    g_print ("Remove Options:\n");
    g_print ("  -a, --account\t\tThe account name (mandatory)\n");
    g_print ("  -i, --issuer\t\tThe issuer name (optional)\n");
    g_print ("  -m, --match-exactly\tShow the token only if it matches exactly the account and/or the issuer (optional)\n");
    g_print ("  -n, --next\tShow also the next token, not only the current one (optional, valid only for TOTP)\n");
    g_print ("\n");
}
