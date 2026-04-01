package com.otpclient.android.core.importexport

import com.otpclient.android.core.crypto.AesGcmCipher
import com.otpclient.android.core.crypto.KeyDerivation
import com.otpclient.android.core.model.OtpEntry
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import java.io.File
import java.security.SecureRandom

object AuthProProvider {

    private const val HEADER = "AUTHENTICATORPRO"
    private const val SALT_SIZE = 16
    private const val IV_SIZE = 12
    private const val TAG_SIZE = 16
    // Argon2id params for AuthPro
    private const val ARGON2_ITER = 3
    private const val ARGON2_MEMCOST = 65536 // 64 MiB
    private const val ARGON2_PARALLEL = 4

    private val json = Json { ignoreUnknownKeys = true }

    fun import(path: String, password: String?): List<OtpEntry> {
        return if (password != null) {
            importEncrypted(path, password)
        } else {
            importPlain(path)
        }
    }

    fun export(path: String, password: String?, entries: List<OtpEntry>) {
        val jsonData = buildAuthProJson(entries)
        if (password != null) {
            exportEncrypted(path, password, jsonData)
        } else {
            File(path).writeText(jsonData, Charsets.UTF_8)
        }
    }

    private fun importPlain(path: String): List<OtpEntry> {
        val content = File(path).readText(Charsets.UTF_8)
        val root = json.parseToJsonElement(content).jsonObject
        val auths = root["Authenticators"]?.jsonArray ?: throw ImportExportException("Missing 'Authenticators'")
        return parseAuthenticators(auths.map { it.jsonObject })
    }

    private fun importEncrypted(path: String, password: String): List<OtpEntry> {
        val fileBytes = File(path).readBytes()

        // Verify header
        val header = String(fileBytes, 0, 16, Charsets.US_ASCII)
        if (header != HEADER) throw ImportExportException("Invalid AuthenticatorPro file header")

        val salt = fileBytes.copyOfRange(16, 16 + SALT_SIZE)
        val iv = fileBytes.copyOfRange(16 + SALT_SIZE, 16 + SALT_SIZE + IV_SIZE)
        val tag = fileBytes.copyOfRange(fileBytes.size - TAG_SIZE, fileBytes.size)
        val ciphertext = fileBytes.copyOfRange(16 + SALT_SIZE + IV_SIZE, fileBytes.size - TAG_SIZE)

        val key = KeyDerivation.deriveKeyArgon2id(
            password = password,
            salt = salt,
            iterations = ARGON2_ITER,
            memoryCostKiB = ARGON2_MEMCOST,
            parallelism = ARGON2_PARALLEL,
        )

        val decrypted = AesGcmCipher.decrypt(
            ciphertext = ciphertext,
            tag = tag,
            key = key,
            iv = iv,
            aad = ByteArray(0),
        )

        val jsonString = String(decrypted, Charsets.UTF_8)
        val root = json.parseToJsonElement(jsonString).jsonObject
        val auths = root["Authenticators"]?.jsonArray ?: throw ImportExportException("Missing 'Authenticators'")
        return parseAuthenticators(auths.map { it.jsonObject })
    }

    private fun parseAuthenticators(auths: List<kotlinx.serialization.json.JsonObject>): List<OtpEntry> {
        return auths.mapNotNull { obj ->
            val typeInt = obj["Type"]?.jsonPrimitive?.int ?: return@mapNotNull null
            val algoInt = obj["Algorithm"]?.jsonPrimitive?.int ?: 0

            val isSteam = typeInt == 4
            val type = when (typeInt) {
                1 -> "HOTP"
                2, 4 -> "TOTP"
                else -> return@mapNotNull null
            }
            val algo = when (algoInt) {
                1 -> "SHA256"
                2 -> "SHA512"
                else -> "SHA1"
            }

            OtpEntry(
                type = type,
                label = obj["Username"]?.jsonPrimitive?.content ?: "",
                issuer = if (isSteam) "Steam" else (obj["Issuer"]?.jsonPrimitive?.content ?: ""),
                secret = obj["Secret"]?.jsonPrimitive?.content?.uppercase() ?: return@mapNotNull null,
                algo = algo,
                digits = obj["Digits"]?.jsonPrimitive?.int ?: 6,
                period = obj["Period"]?.jsonPrimitive?.int ?: 30,
                counter = obj["Counter"]?.jsonPrimitive?.long ?: 0L,
            )
        }
    }

    private fun exportEncrypted(path: String, password: String, jsonData: String) {
        val random = SecureRandom()
        val salt = ByteArray(SALT_SIZE).also { random.nextBytes(it) }
        val iv = ByteArray(IV_SIZE).also { random.nextBytes(it) }

        val key = KeyDerivation.deriveKeyArgon2id(
            password = password,
            salt = salt,
            iterations = ARGON2_ITER,
            memoryCostKiB = ARGON2_MEMCOST,
            parallelism = ARGON2_PARALLEL,
        )

        val encrypted = AesGcmCipher.encrypt(
            plaintext = jsonData.toByteArray(Charsets.UTF_8),
            key = key,
            iv = iv,
            aad = ByteArray(0),
        )

        File(path).outputStream().use { out ->
            out.write(HEADER.toByteArray(Charsets.US_ASCII))
            out.write(salt)
            out.write(iv)
            out.write(encrypted.ciphertext)
            out.write(encrypted.tag)
        }
    }

    private fun buildAuthProJson(entries: List<OtpEntry>): String {
        val auths = entries.map { entry ->
            val isSteam = entry.issuer.equals("Steam", ignoreCase = true)
            val algoInt = when (entry.algo) {
                "SHA256" -> 1
                "SHA512" -> 2
                else -> 0
            }
            val typeInt = when {
                isSteam -> 4
                entry.type == "HOTP" -> 1
                else -> 2
            }
            buildString {
                append("{")
                append("\"Issuer\":${jsonStr(entry.issuer)},")
                append("\"Username\":${jsonStr(entry.label)},")
                append("\"Secret\":${jsonStr(entry.secret)},")
                append("\"Digits\":${entry.digits},")
                append("\"Algorithm\":$algoInt,")
                append("\"Ranking\":0,")
                append("\"Icon\":null,")
                append("\"Pin\":null,")
                append("\"Type\":$typeInt,")
                append("\"Period\":${if (entry.type == "TOTP") entry.period else 0},")
                append("\"Counter\":${if (entry.type == "HOTP") entry.counter else 0}")
                append("}")
            }
        }
        return """{"Authenticators":[${auths.joinToString(",")}],"Categories":[],"AuthenticatorCategories":[],"CustomIcons":[]}"""
    }

    private fun jsonStr(s: String): String {
        return "\"${s.replace("\\", "\\\\").replace("\"", "\\\"")}\""
    }
}
