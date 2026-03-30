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
        "drive-harddisk-symbolic",
        N_("Multiple Databases"),
        N_("You can create and manage multiple encrypted databases.\n\n"
           "Use the sidebar to switch between them. Right-click a database "
           "to rename it, set it as primary, or remove it from the list.\n\n"
           "The primary database is loaded automatically on startup.")
    },
    {
        "security-high-symbolic",
        N_("Security"),
        N_("Secret Service integration can store your database password "
           "in the system keyring so the app unlocks automatically after login. "
           "It is disabled by default.\n\n"
           "You can enable it and configure auto-lock in Settings \u2192 Security.")
    },
    {
        "preferences-system-symbolic",
        N_("Settings & Backup"),
        N_("All preferences are in the Settings dialog.\n\n"
           "You can export and import your settings as JSON from "
           "Settings \u2192 Backup, or via the CLI:\n"
           "  otpclient-cli --export-settings\n"
           "  otpclient-cli --import-settings --file settings.json")
    },
};

/* --- What's New pages (upgrade) --- */

static const PageInfo whats_new_pages[] = {
    {
        "com.github.paolostivanin.OTPClient",
        N_("What\u2019s New"),
        N_("OTPClient has been updated with several improvements.\n\n"
           "Here\u2019s a summary of the key changes.")
    },
    {
        "drive-harddisk-symbolic",
        N_("Multiple Databases"),
        N_("You can now manage multiple encrypted databases from the sidebar.\n\n"
           "Right-click a database to rename it, set it as primary, "
           "or remove it from the list. The primary database loads on startup.")
    },
    {
        "security-high-symbolic",
        N_("Secret Service Changes"),
        N_("Secret Service integration is now disabled by default.\n\n"
           "When enabled, it stores your database password in the system keyring "
           "so the app unlocks automatically. You can toggle it in "
           "Settings \u2192 Security.")
    },
    {
        "preferences-system-symbolic",
        N_("Settings Migration & Export"),
        N_("Configuration has moved from the config file to GSettings "
           "(migrated automatically).\n\n"
           "You can now export and import settings as JSON from "
           "Settings \u2192 Backup, or via the CLI with --export-settings "
           "and --import-settings.")
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
                                         "content-width", 500,
                                         "content-height", 420,
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

    GtkWidget *outer_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);

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

    gtk_box_append (GTK_BOX (outer_box), self->stack);

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

    gtk_box_append (GTK_BOX (outer_box), nav_bar);

    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view), outer_box);
    adw_dialog_set_child (ADW_DIALOG (self), toolbar_view);

    return self;
}
