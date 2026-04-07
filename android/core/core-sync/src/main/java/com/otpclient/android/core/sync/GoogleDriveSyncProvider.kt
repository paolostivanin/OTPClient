package com.otpclient.android.core.sync

import android.content.Context
import com.google.api.client.googleapis.extensions.android.gms.auth.GoogleAccountCredential
import com.google.api.client.http.FileContent
import com.google.api.client.http.javanet.NetHttpTransport
import com.google.api.client.json.gson.GsonFactory
import com.google.api.services.drive.Drive
import com.google.api.services.drive.DriveScopes
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.io.File
import java.util.Collections

class GoogleDriveSyncProvider(
    private val context: Context,
    private val accountEmail: String,
) : SyncProvider {

    private val credential: GoogleAccountCredential by lazy {
        GoogleAccountCredential.usingOAuth2(context, Collections.singleton(DriveScopes.DRIVE_FILE)).apply {
            selectedAccountName = accountEmail
        }
    }

    private val driveService: Drive by lazy {
        Drive.Builder(
            NetHttpTransport(),
            GsonFactory.getDefaultInstance(),
            credential,
        ).setApplicationName("OTPClient").build()
    }

    override suspend fun pull(remoteId: String, localFile: File): SyncResult =
        withContext(Dispatchers.IO) {
            if (!isAuthenticated()) return@withContext SyncResult.NotAuthenticated
            try {
                val driveFileId = findFileByName(remoteId) ?: return@withContext SyncResult.Error("Remote file not found")
                val driveFile = driveService.files().get(driveFileId)
                    .setFields("id,name,modifiedTime")
                    .execute()

                val remoteModified = driveFile.modifiedTime?.value ?: 0L

                if (localFile.exists()) {
                    val localModified = localFile.lastModified()
                    if (localModified > remoteModified && remoteModified > 0) {
                        return@withContext SyncResult.Conflict(localModified, remoteModified)
                    }
                }

                localFile.outputStream().use { output ->
                    driveService.files().get(driveFileId).executeMediaAndDownloadTo(output)
                }
                SyncResult.Success
            } catch (e: java.net.UnknownHostException) {
                SyncResult.NoNetwork
            } catch (e: java.net.ConnectException) {
                SyncResult.NoNetwork
            } catch (e: Exception) {
                SyncResult.Error(e.message ?: "Unknown error")
            }
        }

    override suspend fun push(localFile: File, remoteId: String): SyncResult =
        withContext(Dispatchers.IO) {
            if (!isAuthenticated()) return@withContext SyncResult.NotAuthenticated
            try {
                val mediaContent = FileContent("application/octet-stream", localFile)
                val existingFileId = findFileByName(remoteId)

                if (existingFileId != null) {
                    driveService.files().update(existingFileId, null, mediaContent).execute()
                } else {
                    val fileMetadata = com.google.api.services.drive.model.File().apply {
                        name = remoteId
                        parents = listOf("appDataFolder")
                    }
                    driveService.files().create(fileMetadata, mediaContent)
                        .setFields("id")
                        .execute()
                }
                SyncResult.Success
            } catch (e: java.net.UnknownHostException) {
                SyncResult.NoNetwork
            } catch (e: java.net.ConnectException) {
                SyncResult.NoNetwork
            } catch (e: Exception) {
                SyncResult.Error(e.message ?: "Unknown error")
            }
        }

    override suspend fun getRemoteMetadata(remoteId: String): RemoteFileInfo? =
        withContext(Dispatchers.IO) {
            try {
                val fileId = findFileByName(remoteId) ?: return@withContext null
                val driveFile = driveService.files().get(fileId)
                    .setFields("id,name,modifiedTime,size")
                    .execute()

                RemoteFileInfo(
                    id = driveFile.id,
                    name = driveFile.name,
                    modifiedTime = driveFile.modifiedTime?.value ?: 0L,
                    size = driveFile.getSize()?.toLong() ?: 0L,
                )
            } catch (_: Exception) {
                null
            }
        }

    override fun isAuthenticated(): Boolean {
        return accountEmail.isNotBlank()
    }

    private fun findFileByName(name: String): String? {
        val result = driveService.files().list()
            .setSpaces("appDataFolder")
            .setQ("name = '$name' and trashed = false")
            .setFields("files(id,name)")
            .setPageSize(1)
            .execute()
        return result.files?.firstOrNull()?.id
    }
}
