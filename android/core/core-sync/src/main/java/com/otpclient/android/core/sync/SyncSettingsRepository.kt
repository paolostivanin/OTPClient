package com.otpclient.android.core.sync

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.longPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import androidx.security.crypto.EncryptedSharedPreferences
import androidx.security.crypto.MasterKey
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import javax.inject.Inject
import javax.inject.Singleton

private val Context.syncDataStore: DataStore<Preferences> by preferencesDataStore(name = "sync_settings")

@Singleton
class SyncSettingsRepository @Inject constructor(
    @ApplicationContext private val context: Context,
) {
    private object Keys {
        val PROVIDER_TYPE = stringPreferencesKey("sync_provider_type")
        val LAST_SYNC_TIMESTAMP = longPreferencesKey("last_sync_timestamp")
        val LOCAL_MODIFIED = booleanPreferencesKey("local_modified")
        val GOOGLE_ACCOUNT_EMAIL = stringPreferencesKey("google_account_email")
        val WEBDAV_SERVER_URL = stringPreferencesKey("webdav_server_url")
        val WEBDAV_REMOTE_PATH = stringPreferencesKey("webdav_remote_path")
    }

    private val encryptedPrefs by lazy {
        val masterKey = MasterKey.Builder(context)
            .setKeyScheme(MasterKey.KeyScheme.AES256_GCM)
            .build()
        EncryptedSharedPreferences.create(
            context,
            "sync_credentials",
            masterKey,
            EncryptedSharedPreferences.PrefKeyEncryptionScheme.AES256_SIV,
            EncryptedSharedPreferences.PrefValueEncryptionScheme.AES256_GCM,
        )
    }

    val syncState: Flow<SyncState> = context.syncDataStore.data.map { prefs ->
        SyncState(
            providerType = SyncProviderType.valueOf(
                prefs[Keys.PROVIDER_TYPE] ?: SyncProviderType.NONE.name,
            ),
            lastSyncTimestamp = prefs[Keys.LAST_SYNC_TIMESTAMP] ?: 0L,
            localModified = prefs[Keys.LOCAL_MODIFIED] ?: false,
            googleAccountEmail = prefs[Keys.GOOGLE_ACCOUNT_EMAIL],
            webDavConfig = WebDavConfig(
                serverUrl = prefs[Keys.WEBDAV_SERVER_URL] ?: "",
                username = encryptedPrefs.getString("webdav_username", "") ?: "",
                password = encryptedPrefs.getString("webdav_password", "") ?: "",
                remotePath = prefs[Keys.WEBDAV_REMOTE_PATH] ?: "/otpclient/",
            ),
        )
    }

    suspend fun setProviderType(type: SyncProviderType) {
        context.syncDataStore.edit { it[Keys.PROVIDER_TYPE] = type.name }
    }

    suspend fun setLastSyncTimestamp(timestamp: Long) {
        context.syncDataStore.edit { it[Keys.LAST_SYNC_TIMESTAMP] = timestamp }
    }

    suspend fun setLocalModified(modified: Boolean) {
        context.syncDataStore.edit { it[Keys.LOCAL_MODIFIED] = modified }
    }

    suspend fun setGoogleAccountEmail(email: String?) {
        context.syncDataStore.edit {
            if (email != null) it[Keys.GOOGLE_ACCOUNT_EMAIL] = email
            else it.remove(Keys.GOOGLE_ACCOUNT_EMAIL)
        }
    }

    suspend fun setWebDavConfig(config: WebDavConfig) {
        context.syncDataStore.edit {
            it[Keys.WEBDAV_SERVER_URL] = config.serverUrl
            it[Keys.WEBDAV_REMOTE_PATH] = config.remotePath
        }
        encryptedPrefs.edit()
            .putString("webdav_username", config.username)
            .putString("webdav_password", config.password)
            .apply()
    }
}
