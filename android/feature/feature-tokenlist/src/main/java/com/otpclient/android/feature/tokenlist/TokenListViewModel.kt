package com.otpclient.android.feature.tokenlist

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.otpclient.android.core.database.DatabaseRepository
import com.otpclient.android.core.model.OtpEntry
import com.otpclient.android.core.otp.OtpGenerator
import com.otpclient.android.core.sync.SyncConflict
import com.otpclient.android.core.sync.SyncManager
import com.otpclient.android.core.sync.SyncStatus
import com.otpclient.android.feature.settings.SettingsRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import javax.inject.Inject

@HiltViewModel
class TokenListViewModel @Inject constructor(
    private val databaseRepository: DatabaseRepository,
    private val settingsRepository: SettingsRepository,
    private val syncManager: SyncManager,
) : ViewModel() {

    val pendingConflict: StateFlow<SyncConflict?> = syncManager.pendingConflict
    val syncStatus: StateFlow<SyncStatus> = syncManager.syncStatus

    private val _searchQuery = MutableStateFlow("")
    val searchQuery: StateFlow<String> = _searchQuery

    private val _tickSeconds = MutableStateFlow(System.currentTimeMillis() / 1000)

    val uiState: StateFlow<TokenListUiState> = combine(
        databaseRepository.entries,
        _searchQuery,
        _tickSeconds,
        settingsRepository.settings,
    ) { entries, query, tick, settings ->
        val filtered = if (query.isBlank()) entries else {
            entries.filter { entry ->
                entry.label.contains(query, ignoreCase = true) ||
                    entry.issuer.contains(query, ignoreCase = true)
            }
        }

        val tokens = filtered.mapIndexed { index, entry ->
            val otp = generateOtp(entry, tick)
            val remaining = if (entry.type == "TOTP") {
                OtpGenerator.remainingSeconds(entry.period)
            } else null

            val nextOtp = if (settings.showNextOtp && entry.type == "TOTP") {
                val nextPeriodTime = ((tick / entry.period) + 1) * entry.period
                generateOtp(entry, nextPeriodTime)
            } else null

            TokenUiModel(
                index = entries.indexOf(entry),
                entry = entry,
                currentOtp = otp,
                nextOtp = nextOtp,
                remainingSeconds = remaining,
                period = entry.period,
            )
        }

        TokenListUiState.Loaded(tokens)
    }.stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), TokenListUiState.Loading)

    init {
        // Tick every second to refresh countdowns and OTP values
        viewModelScope.launch {
            while (true) {
                delay(1000)
                _tickSeconds.value = System.currentTimeMillis() / 1000
            }
        }
    }

    fun onSearchQueryChanged(query: String) {
        _searchQuery.value = query
    }

    fun copyOtp(index: Int) {
        // Handled by the UI layer via ClipboardManager
    }

    fun deleteEntry(index: Int) {
        viewModelScope.launch {
            databaseRepository.deleteEntry(index)
        }
    }

    fun incrementHotpCounter(index: Int) {
        viewModelScope.launch {
            databaseRepository.incrementHotpCounter(index)
        }
    }

    fun resolveConflict(keepLocal: Boolean) {
        syncManager.resolveConflict(keepLocal)
    }

    fun lock() {
        databaseRepository.lock()
    }

    private fun generateOtp(entry: OtpEntry, tick: Long): String {
        return when (entry.type) {
            "TOTP" -> {
                if (entry.issuer.equals("Steam", ignoreCase = true)) {
                    OtpGenerator.generateSteam(entry.secret, tick)
                } else {
                    OtpGenerator.generateTotp(
                        secret = entry.secret,
                        period = entry.period,
                        digits = entry.digits,
                        algorithm = entry.algo,
                        timeSeconds = tick,
                    )
                }
            }
            "HOTP" -> OtpGenerator.generateHotp(
                secret = entry.secret,
                counter = entry.counter,
                digits = entry.digits,
                algorithm = entry.algo,
            )
            else -> "------"
        }
    }
}

data class TokenUiModel(
    val index: Int,
    val entry: OtpEntry,
    val currentOtp: String,
    val nextOtp: String?,
    val remainingSeconds: Int?,
    val period: Int,
)

sealed interface TokenListUiState {
    data object Loading : TokenListUiState
    data class Loaded(val tokens: List<TokenUiModel>) : TokenListUiState
}
