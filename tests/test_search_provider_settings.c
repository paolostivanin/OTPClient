/* Use the real D-Bus method handlers with a synthetic cached search result;
 * no keyring, notifications, clipboard tools or real databases are involved. */
#define main search_provider_main
#include "../src/search-provider/search-provider.c"
#undef main

static GDBusConnection *test_bus;
static GVariant *reply;
static GError *call_error;
static gboolean done;

static void
call_done (GObject *source, GAsyncResult *result, gpointer data)
{
    (void) data;
    reply = g_dbus_connection_call_finish (G_DBUS_CONNECTION (source), result, &call_error);
    done = TRUE;
}

static GVariant *
call_method (gboolean kde, const gchar *method, GVariant *args)
{
    done = FALSE;
    reply = NULL;
    g_dbus_connection_call (test_bus, g_dbus_connection_get_unique_name (test_bus),
        kde ? KRUNNER_PATH : GNOME_PATH,
        kde ? "org.kde.krunner1" : "org.gnome.Shell.SearchProvider2",
        method, args, NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, call_done, NULL);
    while (!done) g_main_context_iteration (NULL, TRUE);
    g_assert_no_error (call_error);
    g_assert_nonnull (reply);
    return reply;
}

static void
seed_cached_entry (void)
{
    g_clear_pointer (&cached_entries, g_ptr_array_unref);
    cached_entries = g_ptr_array_new_with_free_func ((GDestroyNotify) otp_search_entry_free);
    OtpSearchEntry *entry = g_new0 (OtpSearchEntry, 1);
    entry->id = g_strdup ("0:0");
    entry->label = g_strdup ("alice");
    entry->issuer = g_strdup ("Example");
    entry->label_fold = g_strdup ("alice");
    entry->issuer_fold = g_strdup ("example");
    entry->db_path = g_strdup ("/synthetic-test.enc");
    g_ptr_array_add (cached_entries, entry);
    cached_at = time (NULL);
}

static void
assert_result_count (gboolean kde, const gchar *query, guint expected)
{
    g_auto (GStrv) terms = g_strsplit (query, " ", -1);
    g_autoptr (GVariant) result = call_method (kde, kde ? "Match" : "GetInitialResultSet",
        kde ? g_variant_new ("(s)", query) : g_variant_new ("(^as)", terms));
    g_autoptr (GVariant) matches = g_variant_get_child_value (result, 0);
    g_assert_cmpuint (g_variant_n_children (matches), ==, expected);
}

static void
test_live_settings (void)
{
    g_settings_set_boolean (provider_settings, "secret-service", TRUE);
    g_settings_set_boolean (provider_settings, "search-provider-enabled", TRUE);
    g_settings_set_string (provider_settings, "search-provider-keyword", "otp");
    seed_cached_entry ();
    assert_result_count (FALSE, "otp alice", 1);
    assert_result_count (TRUE, "otp alice", 1);
    g_assert_cmpuint (g_hash_table_size (g_activation_caps), ==, 2);

    DatabaseData *db = database_data_new ("/synthetic-test.enc", DEFAULT_MEMLOCK_VALUE);
    db->has_cached_key = TRUE;
    db->cached_derived_key = gcry_calloc_secure (1, ARGON2ID_KEYLEN);
    kdf_cache_capture_from_db_data (db, db->db_path);
    database_data_free (db);
    guint64 before = delivery_generation;
    g_settings_set_boolean (provider_settings, "search-provider-enabled", FALSE);
    g_assert_cmpuint (delivery_generation, >, before);
    g_assert_null (cached_entries);
    g_assert_true (g_kdf_cache == NULL || g_hash_table_size (g_kdf_cache) == 0);
    g_assert_true (g_activation_caps == NULL || g_hash_table_size (g_activation_caps) == 0);
    assert_result_count (FALSE, "otp alice", 0);
    assert_result_count (TRUE, "otp alice", 0);
    g_autoptr (GVariant) ignored = call_method (TRUE, "Run", g_variant_new ("(ss)", "old-id", ""));

    g_settings_set_boolean (provider_settings, "search-provider-enabled", TRUE);
    seed_cached_entry ();
    assert_result_count (FALSE, "otp alice", 1);
    g_settings_set_string (provider_settings, "search-provider-keyword", "codes");
    seed_cached_entry ();
    assert_result_count (FALSE, "otp alice", 0);
    assert_result_count (FALSE, "codes alice", 1);
    g_settings_set_boolean (provider_settings, "secret-service", FALSE);
    assert_result_count (FALSE, "codes alice", 0);
    g_assert_null (cached_entries);
}

int
main (int argc, char **argv)
{
    g_test_init (&argc, &argv, NULL);
    g_assert_null (init_libs (DEFAULT_MEMLOCK_VALUE));
    GError *err = NULL;
    test_bus = g_bus_get_sync (G_BUS_TYPE_SESSION, NULL, &err);
    g_assert_no_error (err);
    provider_settings = gsettings_common_get_settings ();
    load_keyword_config ();
    g_signal_connect (provider_settings, "changed", G_CALLBACK (on_provider_settings_changed), NULL);
    GDBusNodeInfo *gnome = g_dbus_node_info_new_for_xml (gnome_introspection_xml, &err);
    g_assert_no_error (err);
    GDBusNodeInfo *kde = g_dbus_node_info_new_for_xml (krunner_introspection_xml, &err);
    g_assert_no_error (err);
    guint gn = g_dbus_connection_register_object (test_bus, GNOME_PATH, gnome->interfaces[0], &g_vtable, NULL, NULL, &err);
    g_assert_no_error (err);
    guint kn = g_dbus_connection_register_object (test_bus, KRUNNER_PATH, kde->interfaces[0], &k_vtable, NULL, NULL, &err);
    g_assert_no_error (err);
    g_test_add_func ("/search-provider/live-settings", test_live_settings);
    int result = g_test_run ();
    g_dbus_connection_unregister_object (test_bus, gn);
    g_dbus_connection_unregister_object (test_bus, kn);
    g_dbus_node_info_unref (gnome);
    g_dbus_node_info_unref (kde);
    g_clear_object (&provider_settings);
    g_clear_object (&test_bus);
    g_free (g_keyword);
    g_free (g_keyword_fold);
    activation_capabilities_clear ();
    kdf_cache_clear ();
    return result;
}
