package com.otpclient.android.core.database

import com.otpclient.android.core.model.Argon2Params
import com.otpclient.android.core.model.CryptoConstants
import com.otpclient.android.core.model.DatabaseInfo
import com.otpclient.android.core.model.OtpEntry
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.withContext
import java.io.File
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class DatabaseRepository @Inject constructor(
    private val databaseManager: DatabaseManager,
) {
    private val _entries = MutableStateFlow<List<OtpEntry>>(emptyList())
    val entries: StateFlow<List<OtpEntry>> = _entries.asStateFlow()

    private var currentPath: String? = null
    private var currentPassword: String? = null

    val isUnlocked: Boolean
        get() = currentPassword != null

    suspend fun unlock(path: String, password: String): Result<List<OtpEntry>> =
        withContext(Dispatchers.IO) {
            try {
                val loaded = databaseManager.loadDatabase(path, password)
                currentPath = path
                currentPassword = password
                _entries.value = loaded
                Result.success(loaded)
            } catch (e: Exception) {
                Result.failure(e)
            }
        }

    suspend fun createAndUnlock(path: String, password: String, params: Argon2Params = Argon2Params()): Result<Unit> =
        withContext(Dispatchers.IO) {
            try {
                databaseManager.createDatabase(path, password, params)
                currentPath = path
                currentPassword = password
                _entries.value = emptyList()
                Result.success(Unit)
            } catch (e: Exception) {
                Result.failure(e)
            }
        }

    suspend fun addEntry(entry: OtpEntry): Result<Unit> = withContext(Dispatchers.IO) {
        try {
            val path = currentPath ?: return@withContext Result.failure(DatabaseException("No database open"))
            val password = currentPassword ?: return@withContext Result.failure(DatabaseException("Database locked"))

            val updated = _entries.value + entry
            databaseManager.saveDatabase(path, password, updated)
            _entries.value = updated
            Result.success(Unit)
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    suspend fun updateEntry(index: Int, entry: OtpEntry): Result<Unit> = withContext(Dispatchers.IO) {
        try {
            val path = currentPath ?: return@withContext Result.failure(DatabaseException("No database open"))
            val password = currentPassword ?: return@withContext Result.failure(DatabaseException("Database locked"))

            val updated = _entries.value.toMutableList()
            updated[index] = entry
            databaseManager.saveDatabase(path, password, updated)
            _entries.value = updated
            Result.success(Unit)
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    suspend fun deleteEntry(index: Int): Result<Unit> = withContext(Dispatchers.IO) {
        try {
            val path = currentPath ?: return@withContext Result.failure(DatabaseException("No database open"))
            val password = currentPassword ?: return@withContext Result.failure(DatabaseException("Database locked"))

            val updated = _entries.value.toMutableList()
            updated.removeAt(index)
            databaseManager.saveDatabase(path, password, updated)
            _entries.value = updated
            Result.success(Unit)
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    suspend fun incrementHotpCounter(index: Int): Result<Unit> = withContext(Dispatchers.IO) {
        try {
            val entry = _entries.value[index]
            if (entry.type != "HOTP") return@withContext Result.failure(DatabaseException("Not an HOTP entry"))
            val updated = entry.copy(counter = entry.counter + 1)
            updateEntry(index, updated)
        } catch (e: Exception) {
            Result.failure(e)
        }
    }

    suspend fun changePassword(newPassword: String, params: Argon2Params = Argon2Params()): Result<Unit> =
        withContext(Dispatchers.IO) {
            try {
                val path = currentPath ?: return@withContext Result.failure(DatabaseException("No database open"))
                val password = currentPassword ?: return@withContext Result.failure(DatabaseException("Database locked"))

                databaseManager.changePassword(path, password, newPassword, params)
                currentPassword = newPassword
                Result.success(Unit)
            } catch (e: Exception) {
                Result.failure(e)
            }
        }

    fun getDatabaseInfo(): DatabaseInfo? {
        val path = currentPath ?: return null
        val file = File(path)
        if (!file.exists()) return null

        val fileBytes = file.readBytes()
        val headerName = String(fileBytes, 0, CryptoConstants.DB_HEADER_NAME_LEN, Charsets.US_ASCII)
        return if (headerName == CryptoConstants.DB_HEADER_NAME) {
            val header = DbHeaderV2.fromByteArray(fileBytes.copyOfRange(0, CryptoConstants.DB_HEADER_V2_SIZE))
            DatabaseInfo(
                name = file.nameWithoutExtension,
                path = path,
                version = header.dbVersion,
                argon2Params = Argon2Params(
                    iterations = header.argon2idIter,
                    memoryCostKiB = header.argon2idMemcost,
                    parallelism = header.argon2idParallelism,
                ),
            )
        } else {
            DatabaseInfo(name = file.nameWithoutExtension, path = path, version = 1)
        }
    }

    fun lock() {
        currentPassword = null
        _entries.value = emptyList()
    }
}
