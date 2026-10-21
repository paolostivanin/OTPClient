#include <glib/gi18n.h>
#include "whats-new-dialog.h"
#include "version.h"

typedef struct {
    const gchar *icon_name;
    const gchar *title;
    const gchar *description;
} PageInfo;

struct _WhatsNewDialog
{
    AdwDialog parent;

    GtkWidget *stack;
    GtkWidget *back_button;
    GtkWidget *next_button;
    GtkWidget *dot_box;

    const PageInfo *pages;
    gint n_pages;
    gint current_page;
};

G_DEFINE_FINAL_TYPE (WhatsNewDialog, whats_new_dialog, ADW_TYPE_DIALOG)

/* --- Welcome pages (first-run) --- */

static const PageInfo welcome_pages[] = {
    {
        "com.github.paolostivanin.OTPClient",
        N_("Welcome to OTPClient"),
        N_("A secure authenticator for TOTP and HOTP tokens.\n\n"
           "Your tokens are stored in encrypted databases protected "
           "by a master password.")
    },
    {
        "view-conceal-symbolic",
        N_("OTPs Are Hidden by Default"),
        N_("Codes are hidden in the list to prevent "
           "shoulder-surfing and accidental screenshot leaks.\n\n"
           "Selecting a row does not reveal or copy anything. Use the "
           "row's Copy button, double-click the row, or press Enter or "
           "Ctrl+C on the selected token, and the code is copied and "
           "briefly shown so you can check it. HOTP rows have a Generate "
           "button instead, because producing a code consumes a counter.\n\n"
           "Turn this off in Settings -> Display to keep TOTP codes "
           "visible at all times.")
    },
    {
        "drive-harddisk-symbolic",
        N_("Multiple Databases"),
        N_("You can create and manage multiple encrypted databases from "
           "the sidebar.\n\n"
           "The first database you create becomes the default - it loads "
           "automatically on startup and is marked with a star. The "
           "currently open database is shown in bold.\n\n"
           "Right-click a database to rename it, set it as the default, "
           "or remove it from the list.")
    },
    {
        "folder-symbolic",
        N_("Organize with Groups"),
        N_("Organize your tokens into groups for quick filtering.\n\n"
           "Use the dropdown in the header bar to filter by group, "
           "or search with the \u201cgroup:\u201d prefix. "
           "Assign groups by right-clicking a token.")
    },
    {
        "system-search-symbolic",
        N_("Keyboard Shortcuts & Search"),
        N_("Use Ctrl+N to add a token, Ctrl+F to search, F2 to edit, "
           "and Ctrl+? to see all shortcuts.\n\n"
           "Desktop search integration lets you find tokens from "
           "GNOME or KDE without opening the app.")
    },
    {
        "security-high-symbolic",
        N_("Security"),
        N_("Secret Service integration can store your database password "
           "in the system keyring so the app unlocks automatically after login. "
           "It is disabled by default.\n\n"
           "You can enable it and configure auto-lock in Settings -> Security.")
    },
    {
        "preferences-system-symbolic",
        N_("Settings & Backup"),
        N_("All preferences are in the Settings dialog, including "
           "countdown colors and clipboard auto-clear.\n\n"
           "Back up and restore both your application preferences and your "
           "token database from Settings -> Backup, or via the CLI with "
           "--export-settings and --import-settings.")
    },
};

/* --- What's New pages (upgrade) --- */

static const PageInfo whats_new_pages[] = {
    {
        "dialog-warning-symbolic",
        N_("Tokens That Need Repair"),
        N_("A token the app cannot accept, such as one with an out-of-range "
           "digit count, is set aside so the rest of the database still "
           "opens. It used to get a single toast and nothing more.\n\n"
           "A banner now counts those tokens, and Review lets you repair or "
           "delete each one. A repair is validated before it replaces the "
           "preserved copy, and a failed repair changes nothing. Deleting "
           "asks first and cannot be undone, because the preserved token "
           "may be the only copy of its secret.")
    },
    {
        "edit-select-all-symbolic",
        N_("Select Several Tokens"),
        N_("Choose Select in the header bar to show a checkbox on every "
           "row. Checked tokens can be assigned to a group, exported, or "
           "deleted, and a bulk delete can be undone from its toast.\n\n"
           "Only checked tokens you can see are affected: changing the "
           "search or the group filter unchecks the ones it hides. Tokens "
           "from other databases that appear in search results are "
           "read-only.")
    },
    {
        "folder-symbolic",
        N_("Manage Groups"),
        N_("Manage Groups, in the main menu, renames a group across all its "
           "tokens at once, or removes the group from them. The tokens "
           "themselves are kept.\n\n"
           "Searching with the \u201cgroup:\u201d prefix now matches every "
           "group whose name contains the text, so group:wo finds Work and "
           "Workshop alike. When nothing matches, Clear Filters resets the "
           "search and the group.")
    },
    {
        "folder-download-symbolic",
        N_("Preview Before Importing"),
        N_("Choosing a file now lists what it contains before anything is "
           "added. Exact duplicates are unchecked and skipped. A token with "
           "the same issuer and account but a different secret is flagged, "
           "so you can decide whether it replaces the old one or is a "
           "second account.\n\n"
           "Uncheck anything you do not want, then choose Import Selected. "
           "Entries the source file could not provide are listed with the "
           "reason.")
    },
    {
        "system-run-symbolic",
        N_("Work Stays in the Background"),
        N_("Reading a backup, writing an export, and changing the database "
           "password or key-derivation settings no longer freeze the window "
           "while they run.\n\n"
           "While a password or key-derivation change runs, the token "
           "actions are disabled, and the app stays open until it finishes. "
           "A password change reports how it ended, and one that finishes "
           "after the app has locked still updates the system keyring.")
    },
    {
        "dialog-information-symbolic",
        N_("Stricter OTP Links"),
        N_("An otpauth:// link is now checked field by field. An HOTP link "
           "with no counter, an unsupported algorithm, or a digit count, "
           "period or counter out of range is refused with the reason, "
           "instead of being imported with a default value.\n\n"
           "This applies to scanned QR codes, links pasted into Add Token, "
           "and imports. A token that is refused can still be added by "
           "hand from its secret.")
    },
};

static void
update_nav_state (WhatsNewDialog *self)
{
    gtk_widget_set_visible (self->back_button, self->current_page > 0);

    if (self->current_page == self->n_pages - 1)
        gtk_button_set_label (GTK_BUTTON (self->next_button), _("Done"));
    else
        gtk_button_set_label (GTK_BUTTON (self->next_button), _("Next"));

    /* Update dot indicators */
    GtkWidget *child = gtk_widget_get_first_child (self->dot_box);
    for (gint i = 0; child != NULL; i++) {
        if (i == self->current_page) {
            gtk_widget_remove_css_class (child, "dim-label");
            gtk_widget_add_css_class (child, "accent");
        } else {
            gtk_widget_remove_css_class (child, "accent");
            gtk_widget_add_css_class (child, "dim-label");
        }
        child = gtk_widget_get_next_sibling (child);
    }

    /* Switch stack page */
    g_autofree gchar *page_name = g_strdup_printf ("page-%d", self->current_page);
    gtk_stack_set_visible_child_name (GTK_STACK (self->stack), page_name);
}

static void
on_back_clicked (GtkButton      *button,
                 WhatsNewDialog *self)
{
    (void) button;
    if (self->current_page > 0) {
        self->current_page--;
        update_nav_state (self);
    }
}

static void
on_next_clicked (GtkButton      *button,
                 WhatsNewDialog *self)
{
    (void) button;
    if (self->current_page < self->n_pages - 1) {
        self->current_page++;
        update_nav_state (self);
    } else {
        adw_dialog_close (ADW_DIALOG (self));
    }
}

static void
whats_new_dialog_init (WhatsNewDialog *self)
{
    (void) self;
}

static void
whats_new_dialog_class_init (WhatsNewDialogClass *klass)
{
    (void) klass;
}

WhatsNewDialog *
whats_new_dialog_new (gboolean is_welcome)
{
    WhatsNewDialog *self = g_object_new (WHATS_NEW_TYPE_DIALOG,
                                         "title", "",
                                         "content-width", 560,
                                         "content-height", 560,
                                         NULL);

    if (is_welcome) {
        self->pages = welcome_pages;
        self->n_pages = G_N_ELEMENTS (welcome_pages);
    } else {
        self->pages = whats_new_pages;
        self->n_pages = G_N_ELEMENTS (whats_new_pages);
    }
    self->current_page = 0;

    /* Build UI */
    GtkWidget *toolbar_view = adw_toolbar_view_new ();
    GtkWidget *header = adw_header_bar_new ();
    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);

    /* Stack with pages */
    self->stack = gtk_stack_new ();
    gtk_stack_set_transition_type (GTK_STACK (self->stack),
                                   GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_set_transition_duration (GTK_STACK (self->stack), 200);
    gtk_widget_set_vexpand (self->stack, TRUE);

    for (gint i = 0; i < self->n_pages; i++) {
        GtkWidget *status_page = adw_status_page_new ();
        adw_status_page_set_icon_name (ADW_STATUS_PAGE (status_page),
                                       self->pages[i].icon_name);
        adw_status_page_set_title (ADW_STATUS_PAGE (status_page),
                                    _(self->pages[i].title));
        adw_status_page_set_description (ADW_STATUS_PAGE (status_page),
                                          _(self->pages[i].description));

        g_autofree gchar *page_name = g_strdup_printf ("page-%d", i);
        gtk_stack_add_named (GTK_STACK (self->stack), status_page, page_name);
    }

    /* Bottom navigation bar */
    GtkWidget *nav_bar = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_halign (nav_bar, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom (nav_bar, 18);
    gtk_widget_set_margin_top (nav_bar, 6);

    self->back_button = gtk_button_new_with_label (_("Back"));
    gtk_widget_add_css_class (self->back_button, "pill");
    gtk_widget_set_visible (self->back_button, FALSE);
    g_signal_connect (self->back_button, "clicked",
                      G_CALLBACK (on_back_clicked), self);

    /* Dot indicators */
    self->dot_box = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_valign (self->dot_box, GTK_ALIGN_CENTER);
    for (gint i = 0; i < self->n_pages; i++) {
        GtkWidget *dot = gtk_label_new ("\u2022");
        gtk_widget_add_css_class (dot, i == 0 ? "accent" : "dim-label");
        gtk_box_append (GTK_BOX (self->dot_box), dot);
    }

    self->next_button = gtk_button_new_with_label (_("Next"));
    gtk_widget_add_css_class (self->next_button, "suggested-action");
    gtk_widget_add_css_class (self->next_button, "pill");
    g_signal_connect (self->next_button, "clicked",
                      G_CALLBACK (on_next_clicked), self);

    gtk_box_append (GTK_BOX (nav_bar), self->back_button);
    gtk_box_append (GTK_BOX (nav_bar), self->dot_box);
    gtk_box_append (GTK_BOX (nav_bar), self->next_button);

    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), self->stack);
    adw_toolbar_view_add_bottom_bar (ADW_TOOLBAR_VIEW (toolbar_view), nav_bar);
    adw_dialog_set_child (ADW_DIALOG (self), toolbar_view);

    return self;
}
