package com.otpclient.android.core.sync

import java.io.File

interface SyncProvider {
    suspend fun pull(remoteId: String, localFile: File): SyncResult
    suspend fun push(localFile: File, remoteId: String): SyncResult
    suspend fun getRemoteMetadata(remoteId: String): RemoteFileInfo?
    fun isAuthenticated(): Boolean
}

data class RemoteFileInfo(
    val id: String,
    val name: String,
    val modifiedTime: Long,
    val size: Long,
)

sealed interface SyncResult {
    data object Success : SyncResult
    data object NoNetwork : SyncResult
    data object NotAuthenticated : SyncResult
    data class Conflict(val localModified: Long, val remoteModified: Long) : SyncResult
    data class Error(val message: String) : SyncResult
}
