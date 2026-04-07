package com.otpclient.android.core.sync

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.Credentials
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.RequestBody.Companion.asRequestBody
import okhttp3.RequestBody.Companion.toRequestBody
import java.io.File
import java.text.SimpleDateFormat
import java.util.Locale
import java.util.TimeZone
import java.util.concurrent.TimeUnit

class WebDavSyncProvider(
    private val config: WebDavConfig,
) : SyncProvider {

    private val client = OkHttpClient.Builder()
        .connectTimeout(30, TimeUnit.SECONDS)
        .readTimeout(60, TimeUnit.SECONDS)
        .writeTimeout(60, TimeUnit.SECONDS)
        .build()

    private val credential = Credentials.basic(config.username, config.password)

    private fun remoteUrl(remoteId: String): String {
        val base = config.serverUrl.trimEnd('/')
        val path = config.remotePath.trimEnd('/')
        return "$base$path/$remoteId"
    }

    override suspend fun pull(remoteId: String, localFile: File): SyncResult =
        withContext(Dispatchers.IO) {
            if (!isAuthenticated()) return@withContext SyncResult.NotAuthenticated
            try {
                val remoteMeta = getRemoteMetadata(remoteId)
                    ?: return@withContext SyncResult.Error("Remote file not found")

                if (localFile.exists()) {
                    val localModified = localFile.lastModified()
                    if (localModified > remoteMeta.modifiedTime && remoteMeta.modifiedTime > 0) {
                        return@withContext SyncResult.Conflict(localModified, remoteMeta.modifiedTime)
                    }
                }

                val request = Request.Builder()
                    .url(remoteUrl(remoteId))
                    .header("Authorization", credential)
                    .get()
                    .build()

                val response = client.newCall(request).execute()
                if (!response.isSuccessful) {
                    return@withContext SyncResult.Error("Download failed: ${response.code}")
                }

                response.body?.byteStream()?.use { input ->
                    localFile.outputStream().use { output ->
                        input.copyTo(output)
                    }
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
                ensureRemoteDirectory()

                val request = Request.Builder()
                    .url(remoteUrl(remoteId))
                    .header("Authorization", credential)
                    .put(localFile.asRequestBody("application/octet-stream".toMediaType()))
                    .build()

                val response = client.newCall(request).execute()
                if (!response.isSuccessful && response.code != 201 && response.code != 204) {
                    return@withContext SyncResult.Error("Upload failed: ${response.code}")
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
                val propfindBody = """
                    <?xml version="1.0" encoding="utf-8"?>
                    <d:propfind xmlns:d="DAV:">
                        <d:prop>
                            <d:getlastmodified/>
                            <d:getcontentlength/>
                        </d:prop>
                    </d:propfind>
                """.trimIndent()

                val request = Request.Builder()
                    .url(remoteUrl(remoteId))
                    .header("Authorization", credential)
                    .header("Depth", "0")
                    .method("PROPFIND", propfindBody.toRequestBody("application/xml".toMediaType()))
                    .build()

                val response = client.newCall(request).execute()
                if (!response.isSuccessful && response.code != 207) return@withContext null

                val body = response.body?.string() ?: return@withContext null
                parsePropsFromMultistatus(remoteId, body)
            } catch (_: Exception) {
                null
            }
        }

    override fun isAuthenticated(): Boolean {
        return config.serverUrl.isNotBlank() &&
            config.username.isNotBlank() &&
            config.password.isNotBlank()
    }

    suspend fun testConnection(): Boolean = withContext(Dispatchers.IO) {
        try {
            val propfindBody = """
                <?xml version="1.0" encoding="utf-8"?>
                <d:propfind xmlns:d="DAV:">
                    <d:prop><d:resourcetype/></d:prop>
                </d:propfind>
            """.trimIndent()

            val base = config.serverUrl.trimEnd('/')
            val request = Request.Builder()
                .url(base)
                .header("Authorization", credential)
                .header("Depth", "0")
                .method("PROPFIND", propfindBody.toRequestBody("application/xml".toMediaType()))
                .build()

            val response = client.newCall(request).execute()
            response.isSuccessful || response.code == 207
        } catch (_: Exception) {
            false
        }
    }

    private fun ensureRemoteDirectory() {
        val base = config.serverUrl.trimEnd('/')
        val pathSegments = config.remotePath.trim('/').split('/')
        var currentPath = ""

        for (segment in pathSegments) {
            currentPath += "/$segment"
            val request = Request.Builder()
                .url("$base$currentPath")
                .header("Authorization", credential)
                .method("MKCOL", null)
                .build()
            try {
                client.newCall(request).execute()
                // 201 = created, 405 = already exists — both are fine
            } catch (_: Exception) {
                // Ignore — parent may already exist
            }
        }
    }

    private fun parsePropsFromMultistatus(remoteId: String, xml: String): RemoteFileInfo? {
        // Simple XML parsing — avoids pulling in a full XML parser dependency
        val lastModifiedRegex = Regex("<d:getlastmodified>([^<]+)</d:getlastmodified>", RegexOption.IGNORE_CASE)
        val contentLengthRegex = Regex("<d:getcontentlength>([^<]+)</d:getcontentlength>", RegexOption.IGNORE_CASE)

        val lastModifiedMatch = lastModifiedRegex.find(xml) ?: return null
        val contentLengthMatch = contentLengthRegex.find(xml)

        val dateFormat = SimpleDateFormat("EEE, dd MMM yyyy HH:mm:ss z", Locale.US)
        dateFormat.timeZone = TimeZone.getTimeZone("GMT")

        val modifiedTime = try {
            dateFormat.parse(lastModifiedMatch.groupValues[1])?.time ?: 0L
        } catch (_: Exception) {
            0L
        }

        val size = contentLengthMatch?.groupValues?.get(1)?.toLongOrNull() ?: 0L

        return RemoteFileInfo(
            id = remoteId,
            name = remoteId,
            modifiedTime = modifiedTime,
            size = size,
        )
    }
}
