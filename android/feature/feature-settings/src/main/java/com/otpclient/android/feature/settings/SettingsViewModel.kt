package com.otpclient.android.feature.settings

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.otpclient.android.core.sync.SyncProviderType
import com.otpclient.android.core.sync.SyncSettingsRepository
import com.otpclient.android.core.sync.SyncState
import com.otpclient.android.core.sync.WebDavConfig
import com.otpclient.android.core.sync.WebDavSyncProvider
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import javax.inject.Inject

@HiltViewModel
class SettingsViewModel @Inject constructor(
    private val settingsRepository: SettingsRepository,
    private val syncSettingsRepository: SyncSettingsRepository,
) : ViewModel() {

    val settings: StateFlow<AppSettings> = settingsRepository.settings
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), AppSettings())

    val syncState: StateFlow<SyncState> = syncSettingsRepository.syncState
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), SyncState())

    private val _webDavTestResult = MutableStateFlow<Boolean?>(null)
    val webDavTestResult: StateFlow<Boolean?> = _webDavTestResult.asStateFlow()

    fun setTheme(theme: Theme) {
        viewModelScope.launch { settingsRepository.setTheme(theme) }
    }

    fun setBiometricEnabled(enabled: Boolean) {
        viewModelScope.launch { settingsRepository.setBiometricEnabled(enabled) }
    }

    fun setAutoLockSeconds(seconds: Int) {
        viewModelScope.launch { settingsRepository.setAutoLockSeconds(seconds) }
    }

    fun setShowNextOtp(show: Boolean) {
        viewModelScope.launch { settingsRepository.setShowNextOtp(show) }
    }

    fun setSyncProviderType(type: SyncProviderType) {
        viewModelScope.launch { syncSettingsRepository.setProviderType(type) }
    }

    fun setGoogleAccountEmail(email: String?) {
        viewModelScope.launch { syncSettingsRepository.setGoogleAccountEmail(email) }
    }

    fun saveWebDavConfig(config: WebDavConfig) {
        viewModelScope.launch { syncSettingsRepository.setWebDavConfig(config) }
    }

    fun testWebDavConnection(config: WebDavConfig) {
        viewModelScope.launch {
            _webDavTestResult.value = null
            val provider = WebDavSyncProvider(config)
            _webDavTestResult.value = provider.testConnection()
        }
    }

    fun clearWebDavTestResult() {
        _webDavTestResult.value = null
    }
}
