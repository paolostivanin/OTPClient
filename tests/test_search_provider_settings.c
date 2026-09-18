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

static json_t *
make_token (const gchar *label,
            const gchar *issuer,
            const gchar *secret)
{
    return build_json_obj ("TOTP", label, issuer, secret, 6, "SHA1", 30, 0, NULL);
}

/* issuer+label alone is not unique: two different secrets may share a name.
 * The identity must distinguish them so selecting the second cannot return the
 * first token's OTP. */
static void
test_token_identity_unambiguous (void)
{
    json_t *root = json_array ();
    json_array_append_new (root, make_token ("alice", "Example", "JBSWY3DPEHPK3PXP"));
    json_array_append_new (root, make_token ("alice", "Example", "KRSXG5CTMVRXEZLU"));

    json_t *a = json_array_get (root, 0);
    json_t *b = json_array_get (root, 1);
    g_autofree gchar *ida = token_identity_from_obj (a);
    g_autofree gchar *idb = token_identity_from_obj (b);
    g_assert_nonnull (ida);
    g_assert_nonnull (idb);
    g_assert_cmpstr (ida, !=, idb);
    g_assert_true (token_obj_matches_identity (a, ida));
    g_assert_true (token_obj_matches_identity (b, idb));
    g_assert_false (token_obj_matches_identity (a, idb));
    g_assert_false (token_obj_matches_identity (b, ida));

    g_autofree gchar *failure = NULL;
    /* Selecting the second token (index 1) resolves to the second, not the
     * first, even though both share issuer+label. */
    g_assert_true (find_token_by_identity (root, idb, 1, &failure) == b);
    g_assert_null (failure);
    /* A stale index that now points at the first token must not win: the scan
     * still finds the second by its unambiguous identity. */
    g_assert_true (find_token_by_identity (root, idb, 0, &failure) == b);
    g_assert_null (failure);

    json_decref (root);
}

/* Two tokens with the same identity (a hash collision, or literally identical
 * entries) must be rejected when the preferred index no longer disambiguates
 * them - returning an arbitrary one could deliver the wrong OTP. */
static void
test_token_identity_ambiguous_rejected (void)
{
    json_t *root = json_array ();
    json_array_append_new (root, make_token ("alice", "Example", "JBSWY3DPEHPK3PXP"));
    json_array_append_new (root, make_token ("alice", "Example", "JBSWY3DPEHPK3PXP"));

    g_autofree gchar *id = token_identity_from_obj (json_array_get (root, 0));
    g_autofree gchar *failure = NULL;
    /* Index out of range forces the scan, which matches both entries. */
    g_assert_null (find_token_by_identity (root, id, 99, &failure));
    g_assert_nonnull (failure);

    json_decref (root);
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
    g_test_add_func ("/search-provider/token-identity", test_token_identity_unambiguous);
    g_test_add_func ("/search-provider/token-identity-ambiguous", test_token_identity_ambiguous_rejected);
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
