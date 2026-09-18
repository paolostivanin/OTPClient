#pragma once

#include "otpclient-window.h"
#include <jansson.h>

G_BEGIN_DECLS


/* HOTP has no periodic rotation, so a reveal cannot be tied to a validity
 * window. Use a short fixed timeout that matches the typical use case
 * (paste the code somewhere, then it disappears). */
#define HOTP_REVEAL_SECONDS 30

#define BACKUP_AGE_WARN_DAYS 30
#define BACKUP_BANNER_SNOOZE_DAYS 7

typedef enum
{
    OTP_COLUMN_ACCOUNT,
    OTP_COLUMN_ISSUER,
    OTP_COLUMN_VALUE
} OTPColumn;

typedef struct
{
    GtkWidget *label;
    GtkWidget *level_bar;
    GtkWidget *box;
    /* Registered for the display; must be removed and unreffed when the row
     * dies or every level bar ever rendered leaks a provider. */
    GtkCssProvider *css_provider;
    guint timeout_id;
    guint remaining;
    guint period;
} ValidityWidgets;

struct _OTPClientWindow
{
    AdwApplicationWindow parent;

    GSettings *settings;
    GtkWidget *toast_overlay;
    GtkWidget *split_view;
    GtkWidget *sidebar_toggle_button;
    GtkWidget *add_button;
    GtkWidget *search_bar;
    GtkWidget *search_entry;
    GtkWidget *lock_button;
    GtkWidget *settings_button;
    GtkWidget *backup_age_banner;
    GtkWidget *database_list;
    GtkWidget *new_db_button;
    GtkWidget *open_db_button;
    GtkWidget *no_db_create_button;
    GtkWidget *no_db_open_button;
    GtkWidget *empty_state_add_button;
    GtkWidget *otp_list;
    GtkWidget *content_stack;
    GtkWidget *loading_status_page;
    GtkWidget *locked_status_page;
    GtkWidget *locked_unlock_button;
    GListStore *otp_store;
    GtkFilterListModel *filter_model;
    GtkCustomFilter *search_filter;
    GtkSortListModel *sort_model;
    GtkSingleSelection *otp_selection;

    GListStore *db_store;

    guint otp_refresh_timer_id;

    /* Drag-and-drop state */
    GtkWidget *dnd_highlight_row;
    GtkCssProvider *dnd_css_provider;

    /* Group filter */
    GtkWidget *group_dropdown;
    GtkStringList *group_list_model;
    gchar *active_group_filter;  /* NULL = "All", "" = "Ungrouped", non-empty = group name */
    gboolean syncing_group_filter;

    /* Search filter cache: lowered forms of the search box contents, refreshed
     * whenever the filter is invalidated (search text changed / group changed /
     * cross-db load completed). search_filter_func runs once per visible row,
     * so caching here avoids re-lowering the query string N times per keystroke. */
    gchar *search_lower;
    gchar *search_group_lower;

    /* Cross-database search */
    GListStore *cross_db_store;
    GtkFlattenListModel *flatten_model;
    gboolean cross_db_loaded;
    gboolean cross_db_loading;
    GCancellable *cross_db_cancellable;
    GCancellable *webcam_cancellable;
    GCancellable *clipboard_cancellable;
    GCancellable *file_dialog_cancellable;
    gboolean disposing;

    /* Clipboard auto-clear */
    guint clipboard_clear_timer_id;
    GdkContentProvider *clipboard_content;

    /* Tracks which OTPEntry currently owns the clipboard contents. Used by
     * otp_refresh_tick to decide whether a TOTP rotation should auto-recopy
     * the next code (only if the rotating entry is still the clipboard owner)
     * or just hide. Weak so the entry can still be freed normally. */
    GWeakRef clipboard_owner_entry;

    /* Undo delete */
    json_t *deleted_token;
    guint   deleted_token_pos;


};

G_END_DECLS
