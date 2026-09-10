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
           "row's Copy button, or press Enter or Ctrl+C on the selected "
           "token, and the code is copied and briefly shown so you can "
           "check it. HOTP rows have a Generate button instead, because "
           "producing a code consumes a counter.\n\n"
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
        "edit-copy-symbolic",
        N_("Copying Is Now Explicit"),
        N_("Selecting a token used to copy it. Clicking a row, or just "
           "arrowing past one, put a code on the clipboard.\n\n"
           "Selection now only selects. Every row has its own copy button in "
           "the Action column, and the same action heads the row's right-click "
           "menu. Enter or Ctrl+C does it for the selected token while the "
           "list has focus. Enter also works on a search result.")
    },
    {
        "security-high-symbolic",
        N_("HOTP Counters Reach Disk First"),
        N_("HOTP rows have a Generate button rather than Copy, because "
           "producing a code consumes a counter. That counter is now "
           "written to the encrypted database as a single transaction "
           "before the code is shown, so a crash can no longer hand out a "
           "code the database does not know it issued.\n\n"
           "The stored counter now means the next unused code in both the "
           "app and the command line. Existing counters are left untouched, "
           "so if an HOTP account rejects its first code after this update, "
           "generate the next one or resynchronize it with the provider.")
    },
    {
        "edit-paste-symbolic",
        N_("The Clipboard Is Left Alone"),
        N_("Locking or quitting used to wipe the clipboard even if you had "
           "copied something else in the meantime, destroying unrelated "
           "data.\n\n"
           "OTPClient now clears the clipboard only while it still owns "
           "what it put there. Copying anything else cancels the pending "
           "wipe.")
    },
    {
        "system-lock-screen-symbolic",
        N_("Locking Clears More"),
        N_("A displayed QR code, a typed secret, an export password, or an "
           "unlock prompt used to survive a lock, still on screen and still "
           "in memory.\n\n"
           "Locking now closes those dialogs, wipes their fields and QR "
           "codes, and cancels anything still in flight, even when a file "
           "chooser is holding a dialog open.")
    },
    {
        "folder-download-symbolic",
        N_("Clearer Imports, Safer Exports"),
        N_("A partial import used to quietly bring in fewer tokens than the "
           "file contained. It now reports how many entries were skipped "
           "and why, one by one, so you can check before deleting the "
           "original.\n\n"
           "New encrypted migration exports require a password. Exports you "
           "already made without one still import.")
    },
    {
        "drive-harddisk-symbolic",
        N_("Backup Reminders Per Database"),
        N_("The backup reminder tracked a single timestamp that could not "
           "say which database it referred to, and exporting for another "
           "app counted as a backup, which suppressed real reminders.\n\n"
           "History and snoozes are now kept per database, and exports no "
           "longer count. The old timestamp could not be attributed to a "
           "database, so every database starts at \u201cNo backup recorded\u201d "
           "and any active snooze is reset.")
    },
    {
        "preferences-system-symbolic",
        N_("Tray, Startup, and Desktop Search"),
        N_("The tray menu was an empty rectangle everywhere except KDE "
           "Plasma. It works now, and OTPClient can start minimized to the "
           "tray and start at login, from Settings -> Integration.\n\n"
           "Search provider settings apply immediately instead of needing a "
           "logout, and turning the provider off revokes its cached keys "
           "right away. Webcam scanning, copying from search, and database "
           "locking also work inside the Flatpak now.")
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
