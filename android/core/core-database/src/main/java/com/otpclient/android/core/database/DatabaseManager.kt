package com.otpclient.android.core.database

import com.otpclient.android.core.crypto.AesGcmCipher
import com.otpclient.android.core.crypto.KeyDerivation
import com.otpclient.android.core.model.Argon2Params
import com.otpclient.android.core.model.CryptoConstants
import com.otpclient.android.core.model.OtpEntry
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import java.io.File
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class DatabaseManager @Inject constructor() {

    private val json = Json {
        ignoreUnknownKeys = true
        encodeDefaults = true
    }

    fun loadDatabase(path: String, password: String): List<OtpEntry> {
        val file = File(path)
        val fileBytes = file.readBytes()
        val version = detectVersion(fileBytes)

        return when (version) {
            1 -> loadV1(fileBytes, password, path)
            2 -> loadV2(fileBytes, password)
            else -> throw DatabaseException("Unknown database version: $version")
        }
    }

    fun saveDatabase(path: String, password: String, entries: List<OtpEntry>, params: Argon2Params = Argon2Params()) {
        val jsonString = json.encodeToString(entries)
        // Match C behavior: encrypt strlen(json) + 1 (includes null terminator)
        val payload = jsonString.toByteArray(Charsets.UTF_8) + byteArrayOf(0)

        val iv = AesGcmCipher.generateIv(CryptoConstants.IV_SIZE)
        val salt = AesGcmCipher.generateSalt(CryptoConstants.KDF_SALT_SIZE)

        val header = DbHeaderV2(
            iv = iv,
            salt = salt,
            argon2idIter = params.iterations,
            argon2idMemcost = params.memoryCostKiB,
            argon2idParallelism = params.parallelism,
        )
        val headerBytes = header.toByteArray()

        val key = KeyDerivation.deriveKeyArgon2id(
            password = password,
            salt = salt,
            iterations = params.iterations,
            memoryCostKiB = params.memoryCostKiB,
            parallelism = params.parallelism,
            hashLength = CryptoConstants.ARGON2ID_KEYLEN,
        )

        val encrypted = AesGcmCipher.encrypt(
            plaintext = payload,
            key = key,
            iv = iv,
            aad = headerBytes,
        )

        File(path).outputStream().use { out ->
            out.write(headerBytes)
            out.write(encrypted.ciphertext)
            out.write(encrypted.tag)
        }
    }

    fun createDatabase(path: String, password: String, params: Argon2Params = Argon2Params()) {
        saveDatabase(path, password, emptyList(), params)
    }

    fun changePassword(path: String, oldPassword: String, newPassword: String, params: Argon2Params = Argon2Params()) {
        val entries = loadDatabase(path, oldPassword)
        saveDatabase(path, newPassword, entries, params)
    }

    private fun detectVersion(fileBytes: ByteArray): Int {
        if (fileBytes.size < CryptoConstants.DB_HEADER_V1_SIZE + CryptoConstants.TAG_SIZE) {
            throw DatabaseException("File too small to be a valid database")
        }
        val headerName = String(fileBytes, 0, CryptoConstants.DB_HEADER_NAME_LEN, Charsets.US_ASCII)
        return if (headerName == CryptoConstants.DB_HEADER_NAME) 2 else 1
    }

    private fun loadV2(fileBytes: ByteArray, password: String): List<OtpEntry> {
        val headerBytes = fileBytes.copyOfRange(0, CryptoConstants.DB_HEADER_V2_SIZE)
        val header = DbHeaderV2.fromByteArray(headerBytes)

        val tag = fileBytes.copyOfRange(fileBytes.size - CryptoConstants.TAG_SIZE, fileBytes.size)
        val ciphertext = fileBytes.copyOfRange(CryptoConstants.DB_HEADER_V2_SIZE, fileBytes.size - CryptoConstants.TAG_SIZE)

        val key = KeyDerivation.deriveKeyArgon2id(
            password = password,
            salt = header.salt,
            iterations = header.argon2idIter,
            memoryCostKiB = header.argon2idMemcost,
            parallelism = header.argon2idParallelism,
            hashLength = CryptoConstants.ARGON2ID_KEYLEN,
        )

        val decrypted = AesGcmCipher.decrypt(
            ciphertext = ciphertext,
            tag = tag,
            key = key,
            iv = header.iv,
            aad = headerBytes,
        )

        // Strip the trailing null byte that the C code adds
        val jsonBytes = if (decrypted.isNotEmpty() && decrypted.last() == 0.toByte()) {
            decrypted.copyOf(decrypted.size - 1)
        } else {
            decrypted
        }

        val jsonString = String(jsonBytes, Charsets.UTF_8)
        return json.decodeFromString<List<OtpEntry>>(jsonString)
    }

    private fun loadV1(fileBytes: ByteArray, password: String, path: String): List<OtpEntry> {
        val headerBytes = fileBytes.copyOfRange(0, CryptoConstants.DB_HEADER_V1_SIZE)
        val header = DbHeaderV1.fromByteArray(headerBytes)

        val tag = fileBytes.copyOfRange(fileBytes.size - CryptoConstants.TAG_SIZE, fileBytes.size)
        val ciphertext = fileBytes.copyOfRange(CryptoConstants.DB_HEADER_V1_SIZE, fileBytes.size - CryptoConstants.TAG_SIZE)

        val key = KeyDerivation.deriveKeyPbkdf2Sha512(
            password = password,
            salt = header.salt,
            iterations = CryptoConstants.KDF_ITERATIONS,
            keyLengthBits = CryptoConstants.KEY_SIZE * 8,
        )

        val decrypted = AesGcmCipher.decrypt(
            ciphertext = ciphertext,
            tag = tag,
            key = key,
            iv = header.iv,
            aad = headerBytes,
        )

        val jsonBytes = if (decrypted.isNotEmpty() && decrypted.last() == 0.toByte()) {
            decrypted.copyOf(decrypted.size - 1)
        } else {
            decrypted
        }

        val jsonString = String(jsonBytes, Charsets.UTF_8)
        val entries = json.decodeFromString<List<OtpEntry>>(jsonString)

        // Auto-migrate V1 to V2
        saveDatabase(path, password, entries)

        return entries
    }
}

class DatabaseException(message: String, cause: Throwable? = null) : Exception(message, cause)
