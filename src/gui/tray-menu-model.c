#include "tray-menu-model.h"

const gchar tray_menu_model_sni_introspection_xml[] =
    "<node>"
    "  <interface name='org.kde.StatusNotifierItem'>"
    "    <property name='Category' type='s' access='read'/>"
    "    <property name='Id' type='s' access='read'/>"
    "    <property name='Title' type='s' access='read'/>"
    "    <property name='Status' type='s' access='read'/>"
    "    <property name='IconName' type='s' access='read'/>"
    "    <property name='Menu' type='o' access='read'/>"
    "    <property name='ItemIsMenu' type='b' access='read'/>"
    "    <method name='Activate'>"
    "      <arg type='i' name='x' direction='in'/>"
    "      <arg type='i' name='y' direction='in'/>"
    "    </method>"
    "    <method name='SecondaryActivate'>"
    "      <arg type='i' name='x' direction='in'/>"
    "      <arg type='i' name='y' direction='in'/>"
    "    </method>"
    "    <signal name='NewStatus'>"
    "      <arg type='s' name='status'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

const gchar tray_menu_model_dbusmenu_introspection_xml[] =
    "<node>"
    "  <interface name='com.canonical.dbusmenu'>"
    "    <property name='Version' type='u' access='read'/>"
    "    <property name='TextDirection' type='s' access='read'/>"
    "    <property name='Status' type='s' access='read'/>"
    "    <method name='GetLayout'>"
    "      <arg type='i' name='parentId' direction='in'/>"
    "      <arg type='i' name='recursionDepth' direction='in'/>"
    "      <arg type='as' name='propertyNames' direction='in'/>"
    "      <arg type='u' name='revision' direction='out'/>"
    "      <arg type='(ia{sv}av)' name='layout' direction='out'/>"
    "    </method>"
    "    <method name='Event'>"
    "      <arg type='i' name='id' direction='in'/>"
    "      <arg type='s' name='eventId' direction='in'/>"
    "      <arg type='v' name='data' direction='in'/>"
    "      <arg type='u' name='timestamp' direction='in'/>"
    "    </method>"
    "    <method name='AboutToShow'>"
    "      <arg type='i' name='id' direction='in'/>"
    "      <arg type='b' name='needUpdate' direction='out'/>"
    "    </method>"
    "    <method name='GetGroupProperties'>"
    "      <arg type='ai' name='ids' direction='in'/>"
    "      <arg type='as' name='propertyNames' direction='in'/>"
    "      <arg type='a(ia{sv})' name='properties' direction='out'/>"
    "    </method>"
    "    <method name='GetProperty'>"
    "      <arg type='i' name='id' direction='in'/>"
    "      <arg type='s' name='name' direction='in'/>"
    "      <arg type='v' name='value' direction='out'/>"
    "    </method>"
    "    <method name='EventGroup'>"
    "      <arg type='a(isvu)' name='events' direction='in'/>"
    "      <arg type='ai' name='idErrors' direction='out'/>"
    "    </method>"
    "    <method name='AboutToShowGroup'>"
    "      <arg type='ai' name='ids' direction='in'/>"
    "      <arg type='ai' name='updatesNeeded' direction='out'/>"
    "      <arg type='ai' name='idErrors' direction='out'/>"
    "    </method>"
    "    <signal name='LayoutUpdated'>"
    "      <arg type='u' name='revision'/>"
    "      <arg type='i' name='parent'/>"
    "    </signal>"
    "    <signal name='ItemsPropertiesUpdated'>"
    "      <arg type='a(ia{sv})' name='updatedProps'/>"
    "      <arg type='a(ias)' name='removedProps'/>"
    "    </signal>"
    "    <signal name='ItemActivationRequested'>"
    "      <arg type='i' name='id'/>"
    "      <arg type='u' name='timestamp'/>"
    "    </signal>"
    "  </interface>"
    "</node>";

guint32
tray_menu_model_revision (void)
{
    return 1;
}

static const TrayMenuEntry *
find_entry (const TrayMenuEntry *items,
            gsize                n_items,
            gint32               id)
{
    for (gsize i = 0; i < n_items; i++)
    {
        if (items[i].id == id)
            return &items[i];
    }
    return NULL;
}

gboolean
tray_menu_model_has_id (const TrayMenuEntry *items,
                        gsize                n_items,
                        gint32               id)
{
    return id == TRAY_MENU_ROOT_ID || find_entry (items, n_items, id) != NULL;
}

TrayMenuAction
tray_menu_model_action_for_id (const TrayMenuEntry *items,
                               gsize                n_items,
                               gint32               id)
{
    const TrayMenuEntry *entry = find_entry (items, n_items, id);
    return entry != NULL ? entry->action : TRAY_MENU_ACTION_NONE;
}

TrayMenuAction
tray_menu_model_action_for_event (const TrayMenuEntry *items,
                                  gsize                n_items,
                                  gint32               id,
                                  const gchar         *event_id)
{
    if (g_strcmp0 (event_id, "clicked") != 0)
        return TRAY_MENU_ACTION_NONE;

    return tray_menu_model_action_for_id (items, n_items, id);
}

/* The single source of truth for an item's properties, shared by the layout,
 * the group fetch and the single-property lookup. It always emits the item's
 * *full* set: libdbusmenu's client replaces rather than merges what a
 * properties reply carries, so anything left out is actively removed from the
 * item, and a missing label falls back to its own "Label Empty" placeholder. */
static void
add_item_properties (GVariantBuilder     *props,
                     const TrayMenuEntry *items,
                     gsize                n_items,
                     gint32               id)
{
    if (id == TRAY_MENU_ROOT_ID)
    {
        /* Only the root is a submenu. Claiming children-display on a leaf makes
         * libdbusmenu-gtk build an empty child menu for it and route clicks to
         * AboutToShow instead of activating it, i.e. a dead entry. */
        g_variant_builder_add (props, "{sv}", "children-display",
                               g_variant_new_string ("submenu"));
        return;
    }

    const TrayMenuEntry *entry = find_entry (items, n_items, id);
    if (entry == NULL)
        return;

    g_variant_builder_add (props, "{sv}", "type",
                           g_variant_new_string ("standard"));
    g_variant_builder_add (props, "{sv}", "label",
                           g_variant_new_string (entry->label != NULL ? entry->label : ""));
    g_variant_builder_add (props, "{sv}", "enabled",
                           g_variant_new_boolean (TRUE));
    g_variant_builder_add (props, "{sv}", "visible",
                           g_variant_new_boolean (TRUE));
}

/* propertyNames is deliberately not honoured anywhere in this file. Sending a
 * superset is always allowed, every studied client copes, and KDE's
 * libdbusmenuqt builds its items purely from the properties GetLayout returns
 * inline, so filtering is what would break it: GNOME asks for
 * ['type','children-display'] and would then never see a label. */
static GVariant *
build_item (const TrayMenuEntry *items,
            gsize                n_items,
            gint32               id,
            gboolean             with_children)
{
    GVariantBuilder props;
    g_variant_builder_init (&props, G_VARIANT_TYPE ("a{sv}"));
    add_item_properties (&props, items, n_items, id);

    GVariantBuilder children;
    g_variant_builder_init (&children, G_VARIANT_TYPE ("av"));

    if (id == TRAY_MENU_ROOT_ID && with_children)
    {
        for (gsize i = 0; i < n_items; i++)
        {
            /* The tree is two levels deep, so a child never has children of its
             * own and the remaining depth does not need threading through. */
            g_variant_builder_add (&children, "v",
                                   build_item (items, n_items, items[i].id, FALSE));
        }
    }

    return g_variant_new ("(ia{sv}av)", id, &props, &children);
}

GVariant *
tray_menu_model_layout (const TrayMenuEntry *items,
                        gsize                n_items,
                        gint32               parent_id,
                        gint32               recursion_depth,
                        gboolean            *found)
{
    if (!tray_menu_model_has_id (items, n_items, parent_id))
    {
        if (found != NULL)
            *found = FALSE;
        return NULL;
    }

    if (found != NULL)
        *found = TRUE;

    /* Per the spec, 0 means the node on its own and a negative depth means no
     * limit. Our tree bottoms out after one level either way. */
    GVariant *item = build_item (items, n_items, parent_id, recursion_depth != 0);

    return g_variant_new ("(u@(ia{sv}av))", tray_menu_model_revision (), item);
}

static void
append_item_properties (GVariantBuilder     *out,
                        const TrayMenuEntry *items,
                        gsize                n_items,
                        gint32               id)
{
    if (!tray_menu_model_has_id (items, n_items, id))
        return;

    GVariantBuilder props;
    g_variant_builder_init (&props, G_VARIANT_TYPE ("a{sv}"));
    add_item_properties (&props, items, n_items, id);

    g_variant_builder_add (out, "(ia{sv})", id, &props);
}

GVariant *
tray_menu_model_group_properties (const TrayMenuEntry *items,
                                  gsize                n_items,
                                  GVariant            *ids)
{
    GVariantBuilder out;
    g_variant_builder_init (&out, G_VARIANT_TYPE ("a(ia{sv})"));

    if (ids == NULL || g_variant_n_children (ids) == 0)
    {
        /* Per the spec, an empty id list means every item. The root has to be
         * in there: libdbusmenu queues a properties fetch for id 0 as well, and
         * an id it asked for but did not get back is dropped from the tree. */
        append_item_properties (&out, items, n_items, TRAY_MENU_ROOT_ID);
        for (gsize i = 0; i < n_items; i++)
            append_item_properties (&out, items, n_items, items[i].id);
    }
    else
    {
        GVariantIter iter;
        gint32 id;

        g_variant_iter_init (&iter, ids);
        while (g_variant_iter_next (&iter, "i", &id))
            append_item_properties (&out, items, n_items, id);
    }

    return g_variant_new ("(a(ia{sv}))", &out);
}

GVariant *
tray_menu_model_property (const TrayMenuEntry *items,
                          gsize                n_items,
                          gint32               id,
                          const gchar         *name)
{
    if (!tray_menu_model_has_id (items, n_items, id))
        return NULL;

    GVariantBuilder props;
    g_variant_builder_init (&props, G_VARIANT_TYPE ("a{sv}"));
    add_item_properties (&props, items, n_items, id);

    g_autoptr (GVariant) dict = g_variant_ref_sink (g_variant_builder_end (&props));

    return g_variant_lookup_value (dict, name, NULL);
}

GVariant *
tray_menu_model_about_to_show_group (const TrayMenuEntry *items,
                                     gsize                n_items,
                                     GVariant            *ids)
{
    GVariantBuilder updates, errors;
    g_variant_builder_init (&updates, G_VARIANT_TYPE ("ai"));
    g_variant_builder_init (&errors, G_VARIANT_TYPE ("ai"));

    if (ids != NULL)
    {
        GVariantIter iter;
        gint32 id;

        g_variant_iter_init (&iter, ids);
        while (g_variant_iter_next (&iter, "i", &id))
        {
            if (!tray_menu_model_has_id (items, n_items, id))
                g_variant_builder_add (&errors, "i", id);
        }
    }

    /* The menu is static, so no item ever needs a layout refresh before it is
     * shown and updatesNeeded stays empty. */
    return g_variant_new ("(aiai)", &updates, &errors);
}

GVariant *
tray_menu_model_event_group (const TrayMenuEntry *items,
                             gsize                n_items,
                             GVariant            *events,
                             GArray              *out_actions)
{
    GVariantBuilder errors;
    g_variant_builder_init (&errors, G_VARIANT_TYPE ("ai"));

    if (events != NULL)
    {
        GVariantIter iter;
        GVariant *child = NULL;

        g_variant_iter_init (&iter, events);
        /* Iterating by value keeps each event tuple alive while event_id
         * borrows from it. */
        while ((child = g_variant_iter_next_value (&iter)) != NULL)
        {
            gint32 id;
            const gchar *event_id;
            g_variant_get (child, "(i&s@vu)", &id, &event_id, NULL, NULL);

            if (!tray_menu_model_has_id (items, n_items, id))
            {
                g_variant_builder_add (&errors, "i", id);
            }
            else
            {
                TrayMenuAction action =
                    tray_menu_model_action_for_event (items, n_items, id, event_id);
                if (action != TRAY_MENU_ACTION_NONE && out_actions != NULL)
                    g_array_append_val (out_actions, action);
            }

            g_variant_unref (child);
        }
    }

    return g_variant_new ("(ai)", &errors);
}
