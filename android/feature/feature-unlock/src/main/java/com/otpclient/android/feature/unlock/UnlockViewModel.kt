package com.otpclient.android.feature.unlock

import android.content.Context
import androidx.fragment.app.FragmentActivity
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.otpclient.android.core.database.DatabaseRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import java.io.File
import javax.inject.Inject

@HiltViewModel
class UnlockViewModel @Inject constructor(
    @ApplicationContext private val context: Context,
    private val databaseRepository: DatabaseRepository,
    private val biometricHelper: BiometricHelper,
) : ViewModel() {

    private val _uiState = MutableStateFlow<UnlockUiState>(UnlockUiState.Idle)
    val uiState: StateFlow<UnlockUiState> = _uiState.asStateFlow()

    fun canUseBiometric(): Boolean = biometricHelper.canUseBiometric()

    fun hasBiometricPassword(dbPath: String): Boolean = biometricHelper.hasStoredPassword(dbPath)

    fun unlock(path: String, password: String) {
        _uiState.value = UnlockUiState.Loading
        viewModelScope.launch {
            val result = databaseRepository.unlock(path, password)
            _uiState.value = result.fold(
                onSuccess = {
                    // Store password for future biometric unlock
                    biometricHelper.storePassword(path, password)
                    UnlockUiState.Success
                },
                onFailure = { UnlockUiState.Error(it.message ?: "Failed to unlock database") },
            )
        }
    }

    fun unlockWithBiometric(activity: FragmentActivity, dbPath: String) {
        biometricHelper.authenticate(
            activity = activity,
            onSuccess = {
                val password = biometricHelper.getStoredPassword(dbPath)
                if (password != null) {
                    unlock(dbPath, password)
                } else {
                    _uiState.value = UnlockUiState.Error(
                        context.getString(R.string.unlock_biometric_no_password),
                    )
                }
            },
            onError = { msg ->
                _uiState.value = UnlockUiState.Error(msg)
            },
        )
    }

    fun createDatabase(path: String, password: String) {
        _uiState.value = UnlockUiState.Loading
        viewModelScope.launch {
            val result = databaseRepository.createAndUnlock(path, password)
            _uiState.value = result.fold(
                onSuccess = {
                    biometricHelper.storePassword(path, password)
                    UnlockUiState.Success
                },
                onFailure = { UnlockUiState.Error(it.message ?: "Failed to create database") },
            )
        }
    }

    fun databaseExists(path: String): Boolean = File(path).exists()
}

sealed interface UnlockUiState {
    data object Idle : UnlockUiState
    data object Loading : UnlockUiState
    data object Success : UnlockUiState
    data class Error(val message: String) : UnlockUiState
}
