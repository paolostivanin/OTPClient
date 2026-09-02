/* The two tests that would actually have caught the empty-tray-menu bug are the
 * first two: introspection completeness and reply-type agreement. Asserting
 * that GetGroupProperties returns two labels only exercises code we just wrote;
 * it cannot catch "we forgot to implement a method", which is the defect that
 * shipped, nor a wrong reply type, which is the one most likely to ship next
 * (g_dbus_method_invocation_return_value logs a mismatch and then sends no
 * reply at all, hanging the host for the full D-Bus timeout). */

#include <glib.h>
#include <gio/gio.h>
#include "tray-menu-model.h"

static const TrayMenuEntry test_items[] = {
    { 1, "Show OTPClient", TRAY_MENU_ACTION_SHOW },
    { 2, "Quit",           TRAY_MENU_ACTION_QUIT },
};
#define N_TEST_ITEMS G_N_ELEMENTS (test_items)

static GDBusInterfaceInfo *
load_interface (const gchar *xml,
                const gchar *name)
{
    GError *err = NULL;
    GDBusNodeInfo *node = g_dbus_node_info_new_for_xml (xml, &err);
    g_assert_no_error (err);
    g_assert_nonnull (node);

    GDBusInterfaceInfo *iface = g_dbus_node_info_lookup_interface (node, name);
    g_assert_nonnull (iface);

    /* The node owns the interface, so keep it alive for the caller. */
    g_dbus_interface_info_ref (iface);
    g_dbus_node_info_unref (node);

    return iface;
}

/* "(u(ia{sv}av))" for GetLayout, and so on: exactly what the reply variant's
 * type string has to be. */
static gchar *
out_signature (GDBusInterfaceInfo *iface,
               const gchar        *method_name)
{
    GDBusMethodInfo *method = g_dbus_interface_info_lookup_method (iface, method_name);
    g_assert_nonnull (method);

    GString *sig = g_string_new ("(");
    for (guint i = 0; method->out_args != NULL && method->out_args[i] != NULL; i++)
        g_string_append (sig, method->out_args[i]->signature);
    g_string_append_c (sig, ')');

    return g_string_free (sig, FALSE);
}

static gchar *
in_signature (GDBusInterfaceInfo *iface,
              const gchar        *method_name)
{
    GDBusMethodInfo *method = g_dbus_interface_info_lookup_method (iface, method_name);
    g_assert_nonnull (method);

    GString *sig = g_string_new ("(");
    for (guint i = 0; method->in_args != NULL && method->in_args[i] != NULL; i++)
        g_string_append (sig, method->in_args[i]->signature);
    g_string_append_c (sig, ')');

    return g_string_free (sig, FALSE);
}

/* --- introspection completeness --- */

static void
test_dbusmenu_declares_every_method (void)
{
    GDBusInterfaceInfo *iface = load_interface (tray_menu_model_dbusmenu_introspection_xml,
                                                "com.canonical.dbusmenu");

    /* All seven. Declaring Version 3 and then omitting the *Group trio is
     * exactly how the menu came out empty on every libdbusmenu host: the
     * client routes to them unconditionally at version >= 3. */
    static const struct { const gchar *name, *in_sig, *out_sig; } expected[] = {
        { "GetLayout",           "(iias)",   "(u(ia{sv}av))" },
        { "GetGroupProperties",  "(aias)",   "(a(ia{sv}))"   },
        { "GetProperty",         "(is)",     "(v)"           },
        { "Event",               "(isvu)",   "()"            },
        { "EventGroup",          "(a(isvu))","(ai)"          },
        { "AboutToShow",         "(i)",      "(b)"           },
        { "AboutToShowGroup",    "(ai)",     "(aiai)"        },
    };

    for (gsize i = 0; i < G_N_ELEMENTS (expected); i++)
    {
        g_assert_nonnull (g_dbus_interface_info_lookup_method (iface, expected[i].name));

        g_autofree gchar *in = in_signature (iface, expected[i].name);
        g_autofree gchar *out = out_signature (iface, expected[i].name);
        g_assert_cmpstr (in, ==, expected[i].in_sig);
        g_assert_cmpstr (out, ==, expected[i].out_sig);
    }

    static const gchar *signals[] = { "LayoutUpdated", "ItemsPropertiesUpdated",
                                      "ItemActivationRequested" };
    for (gsize i = 0; i < G_N_ELEMENTS (signals); i++)
        g_assert_nonnull (g_dbus_interface_info_lookup_signal (iface, signals[i]));

    static const struct { const gchar *name, *sig; } props[] = {
        { "Version", "u" }, { "TextDirection", "s" }, { "Status", "s" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS (props); i++)
    {
        GDBusPropertyInfo *p = g_dbus_interface_info_lookup_property (iface, props[i].name);
        g_assert_nonnull (p);
        g_assert_cmpstr (p->signature, ==, props[i].sig);
    }

    g_dbus_interface_info_unref (iface);
}

static void
test_sni_surface (void)
{
    GDBusInterfaceInfo *iface = load_interface (tray_menu_model_sni_introspection_xml,
                                                "org.kde.StatusNotifierItem");

    /* Every declared property needs a branch in sni_get_property: GDBus asserts
     * that a get_property returning NULL has set the error, so a property in
     * the XML with no matching branch aborts the process on a plain Get(). */
    static const struct { const gchar *name, *sig; } props[] = {
        { "Category", "s" }, { "Id", "s" }, { "Title", "s" }, { "Status", "s" },
        { "IconName", "s" }, { "Menu", "o" }, { "ItemIsMenu", "b" },
    };

    guint n_declared = 0;
    for (guint i = 0; iface->properties != NULL && iface->properties[i] != NULL; i++)
        n_declared++;
    g_assert_cmpuint (n_declared, ==, G_N_ELEMENTS (props));

    for (gsize i = 0; i < G_N_ELEMENTS (props); i++)
    {
        GDBusPropertyInfo *p = g_dbus_interface_info_lookup_property (iface, props[i].name);
        g_assert_nonnull (p);
        g_assert_cmpstr (p->signature, ==, props[i].sig);
    }

    /* Menu must be a real object path. Handing a host "/" makes it build a dead
     * menu and then suppress its own right-click fallback. */
    g_assert_nonnull (g_dbus_interface_info_lookup_method (iface, "Activate"));
    g_assert_nonnull (g_dbus_interface_info_lookup_method (iface, "SecondaryActivate"));

    g_dbus_interface_info_unref (iface);
}

/* --- reply types agree with the declared out-signatures --- */

static void
test_reply_types_match_introspection (void)
{
    GDBusInterfaceInfo *iface = load_interface (tray_menu_model_dbusmenu_introspection_xml,
                                                "com.canonical.dbusmenu");

    gboolean found = FALSE;
    g_autoptr (GVariant) layout =
        g_variant_ref_sink (tray_menu_model_layout (test_items, N_TEST_ITEMS, 0, -1, &found));
    g_autofree gchar *layout_sig = out_signature (iface, "GetLayout");
    g_assert_cmpstr (g_variant_get_type_string (layout), ==, layout_sig);

    g_autoptr (GVariant) ids = g_variant_ref_sink (g_variant_new_parsed ("[0, 1, 2]"));
    g_autoptr (GVariant) group =
        g_variant_ref_sink (tray_menu_model_group_properties (test_items, N_TEST_ITEMS, ids));
    g_autofree gchar *group_sig = out_signature (iface, "GetGroupProperties");
    g_assert_cmpstr (g_variant_get_type_string (group), ==, group_sig);

    g_autoptr (GVariant) ats =
        g_variant_ref_sink (tray_menu_model_about_to_show_group (test_items, N_TEST_ITEMS, ids));
    g_autofree gchar *ats_sig = out_signature (iface, "AboutToShowGroup");
    g_assert_cmpstr (g_variant_get_type_string (ats), ==, ats_sig);

    g_autoptr (GVariant) events =
        g_variant_ref_sink (g_variant_new_parsed ("[(1, 'clicked', <''>, uint32 0)]"));
    g_autoptr (GVariant) errors =
        g_variant_ref_sink (tray_menu_model_event_group (test_items, N_TEST_ITEMS, events, NULL));
    g_autofree gchar *event_sig = out_signature (iface, "EventGroup");
    g_assert_cmpstr (g_variant_get_type_string (errors), ==, event_sig);

    /* GetProperty hands back the bare value; the caller wraps it in "(v)". */
    g_autoptr (GVariant) value =
        tray_menu_model_property (test_items, N_TEST_ITEMS, 1, "label");
    g_assert_nonnull (value);
    g_autoptr (GVariant) wrapped = g_variant_ref_sink (g_variant_new ("(v)", value));
    g_autofree gchar *prop_sig = out_signature (iface, "GetProperty");
    g_assert_cmpstr (g_variant_get_type_string (wrapped), ==, prop_sig);

    g_dbus_interface_info_unref (iface);
}

/* --- layout --- */

static void
assert_leaf_properties (GVariant    *props,
                        const gchar *expected_label)
{
    /* The client replaces rather than merges, so a partial property set removes
     * properties from the item and a missing label falls back to libdbusmenu's
     * own "Label Empty" placeholder. Every reply must carry the full set. */
    g_autoptr (GVariant) type = g_variant_lookup_value (props, "type", G_VARIANT_TYPE_STRING);
    g_autoptr (GVariant) label = g_variant_lookup_value (props, "label", G_VARIANT_TYPE_STRING);
    g_autoptr (GVariant) enabled = g_variant_lookup_value (props, "enabled", G_VARIANT_TYPE_BOOLEAN);
    g_autoptr (GVariant) visible = g_variant_lookup_value (props, "visible", G_VARIANT_TYPE_BOOLEAN);

    g_assert_nonnull (type);
    g_assert_nonnull (label);
    g_assert_nonnull (enabled);
    g_assert_nonnull (visible);

    g_assert_cmpstr (g_variant_get_string (type, NULL), ==, "standard");
    g_assert_cmpstr (g_variant_get_string (label, NULL), ==, expected_label);
    g_assert_true (g_variant_get_boolean (enabled));
    g_assert_true (g_variant_get_boolean (visible));

    /* children-display on a leaf makes libdbusmenu-gtk build an empty child
     * menu and route clicks to AboutToShow instead of activating the item. */
    g_autoptr (GVariant) cd = g_variant_lookup_value (props, "children-display", NULL);
    g_assert_null (cd);
}

static void
test_layout_full_tree (void)
{
    gboolean found = FALSE;
    g_autoptr (GVariant) reply =
        g_variant_ref_sink (tray_menu_model_layout (test_items, N_TEST_ITEMS, 0, -1, &found));
    g_assert_true (found);

    guint32 revision;
    g_autoptr (GVariant) root = NULL;
    g_variant_get (reply, "(u@(ia{sv}av))", &revision, &root);
    g_assert_cmpuint (revision, ==, tray_menu_model_revision ());

    gint32 id;
    g_autoptr (GVariant) props = NULL;
    g_autoptr (GVariant) children = NULL;
    g_variant_get (root, "(i@a{sv}@av)", &id, &props, &children);

    g_assert_cmpint (id, ==, TRAY_MENU_ROOT_ID);

    g_autoptr (GVariant) cd = g_variant_lookup_value (props, "children-display", G_VARIANT_TYPE_STRING);
    g_assert_nonnull (cd);
    g_assert_cmpstr (g_variant_get_string (cd, NULL), ==, "submenu");

    g_assert_cmpuint (g_variant_n_children (children), ==, N_TEST_ITEMS);

    for (gsize i = 0; i < N_TEST_ITEMS; i++)
    {
        g_autoptr (GVariant) boxed = g_variant_get_child_value (children, i);
        g_autoptr (GVariant) child = g_variant_get_variant (boxed);
        g_assert_cmpstr (g_variant_get_type_string (child), ==, "(ia{sv}av)");

        gint32 child_id;
        g_autoptr (GVariant) child_props = NULL;
        g_autoptr (GVariant) grandchildren = NULL;
        g_variant_get (child, "(i@a{sv}@av)", &child_id, &child_props, &grandchildren);

        g_assert_cmpint (child_id, ==, test_items[i].id);
        assert_leaf_properties (child_props, test_items[i].label);
        g_assert_cmpuint (g_variant_n_children (grandchildren), ==, 0);
    }
}

static void
test_layout_depth_zero (void)
{
    gboolean found = FALSE;
    g_autoptr (GVariant) reply =
        g_variant_ref_sink (tray_menu_model_layout (test_items, N_TEST_ITEMS, 0, 0, &found));
    g_assert_true (found);

    g_autoptr (GVariant) root = g_variant_get_child_value (reply, 1);
    g_autoptr (GVariant) children = g_variant_get_child_value (root, 2);
    g_assert_cmpuint (g_variant_n_children (children), ==, 0);
}

static void
test_layout_leaf (void)
{
    gboolean found = FALSE;
    g_autoptr (GVariant) reply =
        g_variant_ref_sink (tray_menu_model_layout (test_items, N_TEST_ITEMS, 2, -1, &found));
    g_assert_true (found);

    g_autoptr (GVariant) node = g_variant_get_child_value (reply, 1);

    gint32 id;
    g_autoptr (GVariant) props = NULL;
    g_autoptr (GVariant) children = NULL;
    g_variant_get (node, "(i@a{sv}@av)", &id, &props, &children);

    g_assert_cmpint (id, ==, 2);
    assert_leaf_properties (props, "Quit");
    g_assert_cmpuint (g_variant_n_children (children), ==, 0);
}

static void
test_layout_unknown_parent (void)
{
    gboolean found = TRUE;
    GVariant *reply = tray_menu_model_layout (test_items, N_TEST_ITEMS, 42, -1, &found);
    g_assert_false (found);
    g_assert_null (reply);
}

/* --- group properties --- */

static GVariant *
group_props_for (const gchar *ids_text)
{
    g_autoptr (GVariant) ids = g_variant_ref_sink (g_variant_new_parsed (ids_text));
    g_autoptr (GVariant) reply =
        g_variant_ref_sink (tray_menu_model_group_properties (test_items, N_TEST_ITEMS, ids));
    return g_variant_get_child_value (reply, 0);
}

static void
test_group_properties_selected (void)
{
    g_autoptr (GVariant) props = group_props_for ("[1, 2]");
    g_assert_cmpuint (g_variant_n_children (props), ==, 2);

    for (gsize i = 0; i < 2; i++)
    {
        gint32 id;
        g_autoptr (GVariant) dict = NULL;
        g_autoptr (GVariant) entry = g_variant_get_child_value (props, i);
        g_variant_get (entry, "(i@a{sv})", &id, &dict);

        g_assert_cmpint (id, ==, test_items[i].id);
        assert_leaf_properties (dict, test_items[i].label);
    }
}

static void
test_group_properties_include_root (void)
{
    /* libdbusmenu does parse_layout_new_child (0, ...) and queues a properties
     * fetch for the root too; an id it asked for and did not get back is
     * dropped, root included. */
    g_autoptr (GVariant) props = group_props_for ("[0, 1, 2]");
    g_assert_cmpuint (g_variant_n_children (props), ==, 3);

    gint32 id;
    g_autoptr (GVariant) dict = NULL;
    g_autoptr (GVariant) entry = g_variant_get_child_value (props, 0);
    g_variant_get (entry, "(i@a{sv})", &id, &dict);

    g_assert_cmpint (id, ==, TRAY_MENU_ROOT_ID);
    g_autoptr (GVariant) cd = g_variant_lookup_value (dict, "children-display", G_VARIANT_TYPE_STRING);
    g_assert_nonnull (cd);
}

static void
test_group_properties_empty_means_all (void)
{
    g_autoptr (GVariant) props = group_props_for ("@ai []");
    g_assert_cmpuint (g_variant_n_children (props), ==, N_TEST_ITEMS + 1);
}

static void
test_group_properties_skips_unknown (void)
{
    g_autoptr (GVariant) props = group_props_for ("[1, 42]");
    g_assert_cmpuint (g_variant_n_children (props), ==, 1);

    gint32 id;
    g_autoptr (GVariant) dict = NULL;
    g_autoptr (GVariant) entry = g_variant_get_child_value (props, 0);
    g_variant_get (entry, "(i@a{sv})", &id, &dict);
    g_assert_cmpint (id, ==, 1);
}

/* --- single property --- */

static void
test_property_lookup (void)
{
    g_autoptr (GVariant) label = tray_menu_model_property (test_items, N_TEST_ITEMS, 1, "label");
    g_assert_nonnull (label);
    g_assert_cmpstr (g_variant_get_string (label, NULL), ==, "Show OTPClient");

    g_autoptr (GVariant) root_cd =
        tray_menu_model_property (test_items, N_TEST_ITEMS, 0, "children-display");
    g_assert_nonnull (root_cd);
    g_assert_cmpstr (g_variant_get_string (root_cd, NULL), ==, "submenu");

    /* Both misses must be NULL so the caller can answer InvalidArgs rather than
     * a "(v)" wrapping nothing. */
    g_assert_null (tray_menu_model_property (test_items, N_TEST_ITEMS, 1, "no-such-property"));
    g_assert_null (tray_menu_model_property (test_items, N_TEST_ITEMS, 42, "label"));
}

/* --- about to show --- */

static void
test_about_to_show_group (void)
{
    g_autoptr (GVariant) ids = g_variant_ref_sink (g_variant_new_parsed ("[0, 42]"));
    g_autoptr (GVariant) reply =
        g_variant_ref_sink (tray_menu_model_about_to_show_group (test_items, N_TEST_ITEMS, ids));

    g_autoptr (GVariant) updates = g_variant_get_child_value (reply, 0);
    g_autoptr (GVariant) errors = g_variant_get_child_value (reply, 1);

    g_assert_cmpuint (g_variant_n_children (updates), ==, 0);
    g_assert_cmpuint (g_variant_n_children (errors), ==, 1);

    gint32 bad;
    g_variant_get_child (errors, 0, "i", &bad);
    g_assert_cmpint (bad, ==, 42);
}

/* --- events --- */

static GVariant *
run_event_group (const gchar *events_text,
                 GArray      *actions)
{
    g_autoptr (GVariant) events = g_variant_ref_sink (g_variant_new_parsed (events_text));
    g_autoptr (GVariant) reply =
        g_variant_ref_sink (tray_menu_model_event_group (test_items, N_TEST_ITEMS, events, actions));
    return g_variant_get_child_value (reply, 0);
}

static void
test_event_group_maps_clicks (void)
{
    g_autoptr (GArray) actions = g_array_new (FALSE, FALSE, sizeof (TrayMenuAction));
    g_autoptr (GVariant) errors =
        run_event_group ("[(2, 'clicked', <''>, uint32 0), (1, 'clicked', <''>, uint32 0)]", actions);

    g_assert_cmpuint (g_variant_n_children (errors), ==, 0);
    g_assert_cmpuint (actions->len, ==, 2);
    /* Order matters: the events are performed in the order the host sent them. */
    g_assert_cmpint (g_array_index (actions, TrayMenuAction, 0), ==, TRAY_MENU_ACTION_QUIT);
    g_assert_cmpint (g_array_index (actions, TrayMenuAction, 1), ==, TRAY_MENU_ACTION_SHOW);
}

static void
test_event_group_ignores_non_clicks (void)
{
    /* KDE brackets the popup with these. Treating them as activations would
     * quit the app just from opening the menu. */
    g_autoptr (GArray) actions = g_array_new (FALSE, FALSE, sizeof (TrayMenuAction));
    g_autoptr (GVariant) errors =
        run_event_group ("[(2, 'opened', <''>, uint32 0), (2, 'closed', <''>, uint32 0),"
                         " (2, 'hovered', <''>, uint32 0)]", actions);

    g_assert_cmpuint (g_variant_n_children (errors), ==, 0);
    g_assert_cmpuint (actions->len, ==, 0);
}

static void
test_event_group_reports_unknown_ids (void)
{
    g_autoptr (GArray) actions = g_array_new (FALSE, FALSE, sizeof (TrayMenuAction));
    g_autoptr (GVariant) errors =
        run_event_group ("[(42, 'clicked', <''>, uint32 0), (1, 'clicked', <''>, uint32 0)]", actions);

    g_assert_cmpuint (g_variant_n_children (errors), ==, 1);
    gint32 bad;
    g_variant_get_child (errors, 0, "i", &bad);
    g_assert_cmpint (bad, ==, 42);

    g_assert_cmpuint (actions->len, ==, 1);
    g_assert_cmpint (g_array_index (actions, TrayMenuAction, 0), ==, TRAY_MENU_ACTION_SHOW);
}

static void
test_action_lookup (void)
{
    g_assert_cmpint (tray_menu_model_action_for_id (test_items, N_TEST_ITEMS, 1), ==, TRAY_MENU_ACTION_SHOW);
    g_assert_cmpint (tray_menu_model_action_for_id (test_items, N_TEST_ITEMS, 2), ==, TRAY_MENU_ACTION_QUIT);
    g_assert_cmpint (tray_menu_model_action_for_id (test_items, N_TEST_ITEMS, 0), ==, TRAY_MENU_ACTION_NONE);
    g_assert_cmpint (tray_menu_model_action_for_id (test_items, N_TEST_ITEMS, 42), ==, TRAY_MENU_ACTION_NONE);

    g_assert_cmpint (tray_menu_model_action_for_event (test_items, N_TEST_ITEMS, 2, "clicked"),
                     ==, TRAY_MENU_ACTION_QUIT);
    g_assert_cmpint (tray_menu_model_action_for_event (test_items, N_TEST_ITEMS, 2, "opened"),
                     ==, TRAY_MENU_ACTION_NONE);
    g_assert_cmpint (tray_menu_model_action_for_event (test_items, N_TEST_ITEMS, 2, NULL),
                     ==, TRAY_MENU_ACTION_NONE);
}

static void
test_has_id (void)
{
    g_assert_true (tray_menu_model_has_id (test_items, N_TEST_ITEMS, TRAY_MENU_ROOT_ID));
    g_assert_true (tray_menu_model_has_id (test_items, N_TEST_ITEMS, 1));
    g_assert_false (tray_menu_model_has_id (test_items, N_TEST_ITEMS, 42));
}

static void
test_revision_is_nonzero (void)
{
    /* LayoutUpdated carrying a revision GetLayout already returned is a no-op
     * on the client, so anything that starts mutating the menu has to bump this
     * before signalling. */
    g_assert_cmpuint (tray_menu_model_revision (), >, 0);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);

    g_test_add_func ("/tray-menu-model/introspection/dbusmenu", test_dbusmenu_declares_every_method);
    g_test_add_func ("/tray-menu-model/introspection/sni", test_sni_surface);
    g_test_add_func ("/tray-menu-model/introspection/reply-types", test_reply_types_match_introspection);

    g_test_add_func ("/tray-menu-model/layout/full-tree", test_layout_full_tree);
    g_test_add_func ("/tray-menu-model/layout/depth-zero", test_layout_depth_zero);
    g_test_add_func ("/tray-menu-model/layout/leaf", test_layout_leaf);
    g_test_add_func ("/tray-menu-model/layout/unknown-parent", test_layout_unknown_parent);

    g_test_add_func ("/tray-menu-model/group-properties/selected", test_group_properties_selected);
    g_test_add_func ("/tray-menu-model/group-properties/root", test_group_properties_include_root);
    g_test_add_func ("/tray-menu-model/group-properties/empty-means-all", test_group_properties_empty_means_all);
    g_test_add_func ("/tray-menu-model/group-properties/unknown", test_group_properties_skips_unknown);

    g_test_add_func ("/tray-menu-model/property", test_property_lookup);
    g_test_add_func ("/tray-menu-model/about-to-show-group", test_about_to_show_group);

    g_test_add_func ("/tray-menu-model/event-group/clicks", test_event_group_maps_clicks);
    g_test_add_func ("/tray-menu-model/event-group/non-clicks", test_event_group_ignores_non_clicks);
    g_test_add_func ("/tray-menu-model/event-group/unknown", test_event_group_reports_unknown_ids);

    g_test_add_func ("/tray-menu-model/action", test_action_lookup);
    g_test_add_func ("/tray-menu-model/has-id", test_has_id);
    g_test_add_func ("/tray-menu-model/revision", test_revision_is_nonzero);

    return g_test_run ();
}
