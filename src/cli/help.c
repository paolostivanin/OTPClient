#include <glib.h>

static void print_main_help         (const gchar *prg_name);

static void print_add_help          (const gchar *prg_name);

static void print_remove_edit_help  (const gchar *prg_name,
                                     const gchar *action);

static void print_show_help         (const gchar *prg_name);

static void print_remove_edit_help  (const gchar *prg_name,
                                     const gchar *action);

static void print_import_help       (const gchar *prg_name);


gint show_help (const gchar *prg_name,
                const gchar *help_command)
{
    if (g_strcmp0 (help_command, "-h") == 0 || g_strcmp0 (help_command, "--help") == 0 ) {
        print_main_help (prg_name);
    } else if (g_strcmp0 (help_command, "--help-add") == 0) {
        print_add_help (prg_name);
    } else if (g_strcmp0 (help_command, "--help-remove") == 0) {
        print_remove_edit_help (prg_name, "remove");
    } else if (g_strcmp0 (help_command, "--help-show") == 0) {
        print_show_help (prg_name);
    } else if (g_strcmp0 (help_command, "--help-edit") == 0) {
        print_remove_edit_help (prg_name, "edit");
    } else if (g_strcmp0 (help_command, "--help-import") == 0) {
        print_import_help (prg_name);
    } else {
        g_print ("Unknown option.\nPlease use '%s --help' to list all available commands.\n", prg_name);
        return -1;
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
    g_print ("  --help-add\t\tShow add options\n");
    g_print ("  --help-remove\t\tShow remove options\n");
    g_print ("  --help-show\t\tShow options\n");
    g_print ("  --help-edit\t\tShow edit options\n");
    g_print ("  --help-import\t\tShow import options\n");
    g_print ("\n");
    g_print ("Main Options:\n");
    g_print ("  -v, --version\t\tShow program version\n");
    g_print ("  --add\t\t\tAdd a token to the database\n");
    g_print ("  --remove\t\tRemove a token from the database\n");
    g_print ("  --show\t\tShow a token\n");
    g_print ("  --list\t\tList all pairs of account and issuer\n");
    g_print ("  --edit\t\tEdit a token\n");
    g_print ("  --import\t\tImport data from either andOTP or Authenticator Plus\n");
    g_print ("  --export\t\tExport data (only andOTP is supported)\n");
    g_print ("\n");
}


static void
print_add_help (const gchar *prg_name)
{
    g_print ("Usage:\n  %s --add [-t ..] [-h ..] <-a ..> [-i ..] [-d ..] [-p ..] [-c ..] [-x]\n", prg_name);
    g_print ("\n");
    g_print ("Add Options:\n");
    g_print ("  -t, --type\t\tEither TOTP or HOTP (optional, default TOTP)\n");
    g_print ("  -h, --hash\t\tEither SHA1, SHA256 or SHA512 (optional, default SHA1)\n");
    g_print ("  -a, --account\t\tThe account name (the only mandatory parameter)\n");
    g_print ("  -i, --issuer\t\tThe issuer name (optional, default empty)\n");
    g_print ("  -d, --digits\t\tEither 6 or 8 (optional, default 6)\n");
    g_print ("  -p, --period\t\tRefresh period in seconds. Can be any value between 10 and 120 (optional, default 30)\n");
    g_print ("  -c, --counter\t\tMandatory if HOTP is used. Value given by the server and not decided by the user\n");
    g_print ("  -x, --show-secret\tShow secret when typing it (optional)\n");
    g_print ("\n");
}


static void
print_remove_edit_help (const gchar *prg_name, const gchar *action)
{
    g_print ("Usage:\n  %s --%s <-a ..> [-i ..] [-m]\n", prg_name, action);
    g_print ("\n");
    g_print ("Remove Options:\n");
    g_print ("  -a, --account\t\tThe account name (mandatory)\n");
    g_print ("  -i, --issuer\t\tThe issuer name (optional)\n");
    g_print ("  -m, --match-exactly\tPerform the action only if it matches exactly the account and/or the issuer (optional)\n");
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
    g_print ("  -n, --next\tShow also the next token, not only the current one (optional)\n");
    g_print ("\n");
}


static void
print_import_help (const gchar *prg_name)
{
    g_print ("Usage:\n  %s --import <-p ..> <-f ..>\n", prg_name);
    g_print ("\n");
    g_print ("Import Options:\n");
    g_print ("  -p, --provider\tEither auth+ or andotp (mandatory)\n");
    g_print ("  -f, --file\t\tThe path to the file that must be imported (mandatory)\n");
    g_print ("\n");
}


