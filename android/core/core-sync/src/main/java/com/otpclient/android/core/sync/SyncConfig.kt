package com.otpclient.android.core.sync

enum class SyncProviderType {
    NONE, GOOGLE_DRIVE, WEBDAV
}

data class WebDavConfig(
    val serverUrl: String = "",
    val username: String = "",
    val password: String = "",
    val remotePath: String = "/otpclient/",
)

data class SyncState(
    val providerType: SyncProviderType = SyncProviderType.NONE,
    val lastSyncTimestamp: Long = 0L,
    val localModified: Boolean = false,
    val googleAccountEmail: String? = null,
    val webDavConfig: WebDavConfig = WebDavConfig(),
)
