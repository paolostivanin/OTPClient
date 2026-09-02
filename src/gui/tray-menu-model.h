#pragma once

#include <glib.h>
#include <gio/gio.h>

G_BEGIN_DECLS

/* The com.canonical.dbusmenu and org.kde.StatusNotifierItem payloads, split out
 * of tray.c so they can be tested without a session bus, a tray host or GTK.
 *
 * Deliberately free of GTK and of otpclient-application.h, and deliberately not
 * wrapped in ENABLE_MINIMIZE_TO_TRAY: guarding it would only produce an empty
 * translation unit and force the test to be conditional too.
 *
 * Labels live in tray.c, not here, so that they can go through _() while this
 * file stays dependency-free and the tests can assert on ids and GVariant types
 * rather than on translated text. */

#define TRAY_MENU_ROOT_ID 0

typedef enum
{
    TRAY_MENU_ACTION_NONE = 0,
    TRAY_MENU_ACTION_SHOW,
    TRAY_MENU_ACTION_QUIT
} TrayMenuAction;

typedef struct
{
    gint32          id;
    const gchar    *label;
    TrayMenuAction  action;
} TrayMenuEntry;

/* Exported so a test can parse them and check that every method we can be
 * called on is declared, and that each builder below produces exactly the
 * out-signature the XML promises. */
extern const gchar tray_menu_model_sni_introspection_xml[];
extern const gchar tray_menu_model_dbusmenu_introspection_xml[];

/* The root counts as existing even though it is not in `items`. */
gboolean        tray_menu_model_has_id              (const TrayMenuEntry *items,
                                                     gsize                n_items,
                                                     gint32               id);

TrayMenuAction  tray_menu_model_action_for_id       (const TrayMenuEntry *items,
                                                     gsize                n_items,
                                                     gint32               id);

/* Applies the eventId filter: only "clicked" maps to an action. KDE also sends
 * "opened" and "closed" around the popup and those are not activations. */
TrayMenuAction  tray_menu_model_action_for_event    (const TrayMenuEntry *items,
                                                     gsize                n_items,
                                                     gint32               id,
                                                     const gchar         *event_id);

/* The layout revision. It must never decrease, and a LayoutUpdated carrying a
 * revision that GetLayout has already returned is a no-op on the client side
 * (libdbusmenu only refetches when current_revision > my_revision). The menu is
 * static, so this is a constant and LayoutUpdated is never emitted; anything
 * that starts changing the menu has to bump it here first. */
guint32         tray_menu_model_revision            (void);

/* All of the builders below return a floating reference, ready to hand to
 * g_dbus_method_invocation_return_value, except tray_menu_model_property which
 * returns a full one. */

/* (u(ia{sv}av)). `found` is set FALSE and NULL returned for an unknown
 * parent_id, which the caller turns into InvalidArgs. recursion_depth follows
 * the spec: 0 means the node alone, anything negative means unlimited.
 * propertyNames is deliberately not a parameter, see the .c. */
GVariant       *tray_menu_model_layout              (const TrayMenuEntry *items,
                                                     gsize                n_items,
                                                     gint32               parent_id,
                                                     gint32               recursion_depth,
                                                     gboolean            *found);

/* (a(ia{sv})). An empty `ids` means every item; unknown ids are skipped. */
GVariant       *tray_menu_model_group_properties    (const TrayMenuEntry *items,
                                                     gsize                n_items,
                                                     GVariant            *ids);

/* The bare property value, transfer full, or NULL when the item or the property
 * does not exist. Not wrapped in the "(v)" reply tuple. */
GVariant       *tray_menu_model_property            (const TrayMenuEntry *items,
                                                     gsize                n_items,
                                                     gint32               id,
                                                     const gchar         *name);

/* (aiai): updatesNeeded then idErrors. Note the two arrays; replying (ai) here
 * would make GDBus log a type mismatch and send no reply at all. */
GVariant       *tray_menu_model_about_to_show_group (const TrayMenuEntry *items,
                                                     gsize                n_items,
                                                     GVariant            *ids);

/* (ai) of the ids we could not handle. Every event that does map to something
 * appends its TrayMenuAction to `out_actions` (a GArray of TrayMenuAction, may
 * be NULL) in call order, so the caller performs the side effects and this
 * stays pure. */
GVariant       *tray_menu_model_event_group         (const TrayMenuEntry *items,
                                                     gsize                n_items,
                                                     GVariant            *events,
                                                     GArray              *out_actions);

G_END_DECLS
