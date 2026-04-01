package com.otpclient.android.feature.importexport

import android.content.Context
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.otpclient.android.core.database.DatabaseRepository
import dagger.hilt.android.qualifiers.ApplicationContext
import com.otpclient.android.core.importexport.AegisProvider
import com.otpclient.android.core.importexport.AuthProProvider
import com.otpclient.android.core.importexport.FreeOtpPlusProvider
import com.otpclient.android.core.importexport.OtpauthUri
import com.otpclient.android.core.importexport.TwoFasProvider
import com.otpclient.android.core.model.OtpEntry
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import javax.inject.Inject

enum class ImportExportFormat(val displayName: String) {
    FREEOTPPLUS("FreeOTP+"),
    AEGIS_PLAIN("Aegis (plain)"),
    AEGIS_ENCRYPTED("Aegis (encrypted)"),
    AUTHPRO_PLAIN("Authenticator Pro (plain)"),
    AUTHPRO_ENCRYPTED("Authenticator Pro (encrypted)"),
    TWOFAS_PLAIN("2FAS (plain)"),
    TWOFAS_ENCRYPTED("2FAS (encrypted)"),
}

val importFormats = ImportExportFormat.entries.toList()
val exportFormats = ImportExportFormat.entries.toList()

@HiltViewModel
class ImportExportViewModel @Inject constructor(
    @ApplicationContext private val context: Context,
    private val databaseRepository: DatabaseRepository,
) : ViewModel() {

    private val _uiState = MutableStateFlow<ImportExportUiState>(ImportExportUiState.Idle)
    val uiState: StateFlow<ImportExportUiState> = _uiState.asStateFlow()

    fun importFrom(path: String, format: ImportExportFormat, password: String?) {
        _uiState.value = ImportExportUiState.Processing
        viewModelScope.launch(Dispatchers.IO) {
            try {
                val entries = when (format) {
                    ImportExportFormat.FREEOTPPLUS -> FreeOtpPlusProvider.import(path)
                    ImportExportFormat.AEGIS_PLAIN -> AegisProvider.import(path, null)
                    ImportExportFormat.AEGIS_ENCRYPTED -> AegisProvider.import(path, password)
                    ImportExportFormat.AUTHPRO_PLAIN -> AuthProProvider.import(path, null)
                    ImportExportFormat.AUTHPRO_ENCRYPTED -> AuthProProvider.import(path, password)
                    ImportExportFormat.TWOFAS_PLAIN -> TwoFasProvider.import(path, null)
                    ImportExportFormat.TWOFAS_ENCRYPTED -> TwoFasProvider.import(path, password)
                }

                var added = 0
                for (entry in entries) {
                    val result = databaseRepository.addEntry(entry)
                    if (result.isSuccess) added++
                }

                _uiState.value = ImportExportUiState.Success(
                    context.getString(R.string.importexport_imported, added),
                )
            } catch (e: Exception) {
                _uiState.value = ImportExportUiState.Error(
                    e.message ?: context.getString(R.string.importexport_import_failed),
                )
            }
        }
    }

    fun exportTo(path: String, format: ImportExportFormat, password: String?) {
        _uiState.value = ImportExportUiState.Processing
        viewModelScope.launch(Dispatchers.IO) {
            try {
                val entries = databaseRepository.entries.value

                when (format) {
                    ImportExportFormat.FREEOTPPLUS -> FreeOtpPlusProvider.export(path, entries)
                    ImportExportFormat.AEGIS_PLAIN -> AegisProvider.export(path, null, entries)
                    ImportExportFormat.AEGIS_ENCRYPTED -> AegisProvider.export(path, password, entries)
                    ImportExportFormat.AUTHPRO_PLAIN -> AuthProProvider.export(path, null, entries)
                    ImportExportFormat.AUTHPRO_ENCRYPTED -> AuthProProvider.export(path, password, entries)
                    ImportExportFormat.TWOFAS_PLAIN -> TwoFasProvider.export(path, null, entries)
                    ImportExportFormat.TWOFAS_ENCRYPTED -> TwoFasProvider.export(path, password, entries)
                }

                _uiState.value = ImportExportUiState.Success(
                    context.getString(R.string.importexport_exported, entries.size),
                )
            } catch (e: Exception) {
                _uiState.value = ImportExportUiState.Error(
                    e.message ?: context.getString(R.string.importexport_export_failed),
                )
            }
        }
    }

    fun resetState() {
        _uiState.value = ImportExportUiState.Idle
    }
}

sealed interface ImportExportUiState {
    data object Idle : ImportExportUiState
    data object Processing : ImportExportUiState
    data class Success(val message: String) : ImportExportUiState
    data class Error(val message: String) : ImportExportUiState
}
