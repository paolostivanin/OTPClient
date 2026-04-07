package com.otpclient.android.core.sync

import android.content.Context
import android.net.ConnectivityManager
import android.net.NetworkCapabilities
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import java.io.File
import javax.inject.Inject
import javax.inject.Singleton

data class SyncConflict(
    val dbPath: String,
    val localModified: Long,
    val remoteModified: Long,
)

enum class SyncStatus {
    IDLE, SYNCING, SYNCED, PENDING, OFFLINE, ERROR
}

@Singleton
class SyncManager @Inject constructor(
    @ApplicationContext private val context: Context,
    private val syncSettingsRepository: SyncSettingsRepository,
) {
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.IO)

    private val _pendingConflict = MutableStateFlow<SyncConflict?>(null)
    val pendingConflict: StateFlow<SyncConflict?> = _pendingConflict.asStateFlow()

    private val _syncStatus = MutableStateFlow(SyncStatus.IDLE)
    val syncStatus: StateFlow<SyncStatus> = _syncStatus.asStateFlow()

    private fun getProvider(state: SyncState): SyncProvider? {
        return when (state.providerType) {
            SyncProviderType.NONE -> null
            SyncProviderType.GOOGLE_DRIVE -> {
                val email = state.googleAccountEmail ?: return null
                GoogleDriveSyncProvider(context, email)
            }
            SyncProviderType.WEBDAV -> {
                if (!state.webDavConfig.serverUrl.isNotBlank()) return null
                WebDavSyncProvider(state.webDavConfig)
            }
        }
    }

    private fun isNetworkAvailable(): Boolean {
        val cm = context.getSystemService(Context.CONNECTIVITY_SERVICE) as ConnectivityManager
        val network = cm.activeNetwork ?: return false
        val caps = cm.getNetworkCapabilities(network) ?: return false
        return caps.hasCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
    }

    fun syncOnOpen(dbPath: String) {
        scope.launch {
            if (!isNetworkAvailable()) {
                _syncStatus.value = SyncStatus.OFFLINE
                return@launch
            }

            val state = syncSettingsRepository.syncState.first()
            val provider = getProvider(state) ?: return@launch

            _syncStatus.value = SyncStatus.SYNCING
            val localFile = File(dbPath)
            val remoteId = localFile.name

            when (val result = provider.pull(remoteId, localFile)) {
                is SyncResult.Success -> {
                    syncSettingsRepository.setLastSyncTimestamp(System.currentTimeMillis())
                    _syncStatus.value = SyncStatus.SYNCED
                }
                is SyncResult.Conflict -> {
                    _pendingConflict.value = SyncConflict(
                        dbPath = dbPath,
                        localModified = result.localModified,
                        remoteModified = result.remoteModified,
                    )
                    _syncStatus.value = SyncStatus.IDLE
                }
                is SyncResult.NoNetwork -> {
                    _syncStatus.value = SyncStatus.OFFLINE
                }
                is SyncResult.NotAuthenticated -> {
                    _syncStatus.value = SyncStatus.ERROR
                }
                is SyncResult.Error -> {
                    // Fail silently on open — user sees local data
                    _syncStatus.value = SyncStatus.IDLE
                }
            }
        }
    }

    fun syncOnSave(dbPath: String) {
        scope.launch {
            syncSettingsRepository.setLocalModified(true)

            if (!isNetworkAvailable()) {
                _syncStatus.value = SyncStatus.PENDING
                return@launch
            }

            val state = syncSettingsRepository.syncState.first()
            val provider = getProvider(state) ?: return@launch

            _syncStatus.value = SyncStatus.SYNCING
            val localFile = File(dbPath)
            val remoteId = localFile.name

            when (provider.push(localFile, remoteId)) {
                is SyncResult.Success -> {
                    syncSettingsRepository.setLocalModified(false)
                    syncSettingsRepository.setLastSyncTimestamp(System.currentTimeMillis())
                    _syncStatus.value = SyncStatus.SYNCED
                }
                is SyncResult.NoNetwork -> {
                    _syncStatus.value = SyncStatus.PENDING
                }
                else -> {
                    _syncStatus.value = SyncStatus.PENDING
                }
            }
        }
    }

    fun resolveConflict(keepLocal: Boolean) {
        val conflict = _pendingConflict.value ?: return
        _pendingConflict.value = null

        scope.launch {
            val state = syncSettingsRepository.syncState.first()
            val provider = getProvider(state) ?: return@launch
            val localFile = File(conflict.dbPath)
            val remoteId = localFile.name

            if (keepLocal) {
                // Push local to remote
                provider.push(localFile, remoteId)
                syncSettingsRepository.setLocalModified(false)
            } else {
                // Force pull remote (re-download without conflict check)
                val request = provider.pull(remoteId, localFile)
                // The pull will overwrite local — caller should reload entries
            }
            syncSettingsRepository.setLastSyncTimestamp(System.currentTimeMillis())
            _syncStatus.value = SyncStatus.SYNCED
        }
    }

    fun retryPendingSync(dbPath: String) {
        scope.launch {
            val state = syncSettingsRepository.syncState.first()
            if (state.localModified && isNetworkAvailable()) {
                syncOnSave(dbPath)
            }
        }
    }
}
