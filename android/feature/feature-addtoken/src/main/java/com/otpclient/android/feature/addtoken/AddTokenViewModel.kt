package com.otpclient.android.feature.addtoken

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.otpclient.android.core.database.DatabaseRepository
import com.otpclient.android.core.model.OtpEntry
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import javax.inject.Inject

@HiltViewModel
class AddTokenViewModel @Inject constructor(
    private val databaseRepository: DatabaseRepository,
) : ViewModel() {

    private val _uiState = MutableStateFlow<AddTokenUiState>(AddTokenUiState.Idle)
    val uiState: StateFlow<AddTokenUiState> = _uiState.asStateFlow()

    fun saveToken(
        type: String,
        issuer: String,
        label: String,
        secret: String,
        algorithm: String,
        digits: Int,
        period: Int,
        counter: Long,
        editIndex: Int? = null,
    ) {
        if (secret.isBlank()) {
            _uiState.value = AddTokenUiState.Error("Secret is required")
            return
        }
        if (label.isBlank() && issuer.isBlank()) {
            _uiState.value = AddTokenUiState.Error("Label or issuer is required")
            return
        }

        val entry = OtpEntry(
            type = type,
            label = label,
            issuer = issuer,
            secret = secret.replace(" ", "").uppercase(),
            algo = algorithm,
            digits = digits,
            period = period,
            counter = counter,
        )

        _uiState.value = AddTokenUiState.Saving
        viewModelScope.launch {
            val result = if (editIndex != null) {
                databaseRepository.updateEntry(editIndex, entry)
            } else {
                databaseRepository.addEntry(entry)
            }
            _uiState.value = result.fold(
                onSuccess = { AddTokenUiState.Success },
                onFailure = { AddTokenUiState.Error(it.message ?: "Failed to save token") },
            )
        }
    }
}

sealed interface AddTokenUiState {
    data object Idle : AddTokenUiState
    data object Saving : AddTokenUiState
    data object Success : AddTokenUiState
    data class Error(val message: String) : AddTokenUiState
}
