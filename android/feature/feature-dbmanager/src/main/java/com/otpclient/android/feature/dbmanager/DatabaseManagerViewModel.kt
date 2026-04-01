package com.otpclient.android.feature.dbmanager

import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.otpclient.android.core.database.DatabaseRepository
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import javax.inject.Inject

@HiltViewModel
class DatabaseManagerViewModel @Inject constructor(
    private val databaseListRepository: DatabaseListRepository,
    private val databaseRepository: DatabaseRepository,
) : ViewModel() {

    val databases: StateFlow<List<DatabaseEntry>> = databaseListRepository.databases
        .stateIn(viewModelScope, SharingStarted.WhileSubscribed(5000), emptyList())

    private val _message = MutableStateFlow<String?>(null)
    val message: StateFlow<String?> = _message.asStateFlow()

    fun addDatabase(name: String, path: String, password: String) {
        viewModelScope.launch {
            try {
                databaseRepository.createAndUnlock(path, password)
                databaseListRepository.addDatabase(DatabaseEntry(name, path))
                databaseRepository.lock()
                _message.value = "Database '$name' created"
            } catch (e: Exception) {
                _message.value = "Failed: ${e.message}"
            }
        }
    }

    fun registerExistingDatabase(name: String, path: String) {
        viewModelScope.launch {
            databaseListRepository.addDatabase(DatabaseEntry(name, path))
            _message.value = "Database '$name' added"
        }
    }

    fun removeDatabase(path: String) {
        viewModelScope.launch {
            databaseListRepository.removeDatabase(path)
            _message.value = "Database removed"
        }
    }

    fun renameDatabase(path: String, newName: String) {
        viewModelScope.launch {
            databaseListRepository.renameDatabase(path, newName)
        }
    }

    fun changePassword(path: String, oldPassword: String, newPassword: String) {
        viewModelScope.launch {
            try {
                databaseRepository.unlock(path, oldPassword)
                databaseRepository.changePassword(newPassword)
                databaseRepository.lock()
                _message.value = "Password changed"
            } catch (e: Exception) {
                _message.value = "Failed: ${e.message}"
            }
        }
    }

    fun clearMessage() {
        _message.value = null
    }
}
