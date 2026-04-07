package com.otpclient.android.core.importexport

import android.util.Base64
import com.otpclient.android.core.crypto.AesGcmCipher
import com.otpclient.android.core.model.OtpEntry
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import org.bouncycastle.crypto.generators.SCrypt
import java.io.File
import java.security.SecureRandom
import java.util.UUID
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

object AegisProvider {

    private const val NONCE_SIZE = 12
    private const val TAG_SIZE = 16
    private const val SALT_SIZE = 32
    private const val KEY_SIZE = 32

    private val json = Json { ignoreUnknownKeys = true }

    fun import(path: String, password: String?): List<OtpEntry> {
        val content = File(path).readText(Charsets.UTF_8)
        val root = json.parseToJsonElement(content).jsonObject

        return if (password != null) {
            importEncrypted(root, password)
        } else {
            importPlain(root)
        }
    }

    fun export(path: String, password: String?, entries: List<OtpEntry>) {
        if (password != null) {
            exportEncrypted(path, password, entries)
        } else {
            exportPlain(path, entries)
        }
    }

    private fun importPlain(root: JsonObject): List<OtpEntry> {
        val db = root["db"]?.jsonObject ?: throw ImportExportException("Missing 'db' field")
        val entriesArray = db["entries"]?.jsonArray ?: throw ImportExportException("Missing 'entries' field")
        return parseEntries(entriesArray.map { it.jsonObject })
    }

    private fun importEncrypted(root: JsonObject, password: String): List<OtpEntry> {
        val header = root["header"]?.jsonObject ?: throw ImportExportException("Missing header")
        val slots = header["slots"]?.jsonArray ?: throw ImportExportException("Missing slots")

        // Find password slot (type == 1)
        val passwordSlot = slots.map { it.jsonObject }.firstOrNull {
            it["type"]?.jsonPrimitive?.int == 1
        } ?: throw ImportExportException("No password slot found")

        val n = passwordSlot["n"]?.jsonPrimitive?.int ?: 32768
        val r = passwordSlot["r"]?.jsonPrimitive?.int ?: 8
        val p = passwordSlot["p"]?.jsonPrimitive?.int ?: 1
        val salt = HexUtils.hexToBytes(passwordSlot["salt"]?.jsonPrimitive?.content ?: throw ImportExportException("Missing salt"))
        val encKey = HexUtils.hexToBytes(passwordSlot["key"]?.jsonPrimitive?.content ?: throw ImportExportException("Missing key"))
        val keyParams = passwordSlot["key_params"]?.jsonObject ?: throw ImportExportException("Missing key_params")
        val keyNonce = HexUtils.hexToBytes(keyParams["nonce"]?.jsonPrimitive?.content ?: throw ImportExportException("Missing nonce"))
        val keyTag = HexUtils.hexToBytes(keyParams["tag"]?.jsonPrimitive?.content ?: throw ImportExportException("Missing tag"))

        // Derive key with Scrypt
        val derivedKey = SCrypt.generate(
            password.toByteArray(Charsets.UTF_8),
            salt, n, r, p, KEY_SIZE,
        )

        try {
            // Decrypt master key
            val masterKey = AesGcmCipher.decrypt(
                ciphertext = encKey,
                tag = keyTag,
                key = derivedKey,
                iv = keyNonce,
                aad = ByteArray(0),
            )

            try {
                // Decrypt database
                val dbParams = header["params"]?.jsonObject ?: throw ImportExportException("Missing params")
                val dbNonce = HexUtils.hexToBytes(dbParams["nonce"]?.jsonPrimitive?.content ?: throw ImportExportException("Missing db nonce"))
                val dbTag = HexUtils.hexToBytes(dbParams["tag"]?.jsonPrimitive?.content ?: throw ImportExportException("Missing db tag"))
                val dbB64 = root["db"]?.jsonPrimitive?.content ?: throw ImportExportException("Missing db data")
                val dbEncrypted = Base64.decode(dbB64, Base64.DEFAULT)

                val dbDecrypted = AesGcmCipher.decrypt(
                    ciphertext = dbEncrypted,
                    tag = dbTag,
                    key = masterKey,
                    iv = dbNonce,
                    aad = ByteArray(0),
                )

                val dbJson = String(dbDecrypted, Charsets.UTF_8)
                val dbObj = json.parseToJsonElement(dbJson).jsonObject
                val entriesArray = dbObj["entries"]?.jsonArray ?: throw ImportExportException("Missing entries")
                return parseEntries(entriesArray.map { it.jsonObject })
            } finally {
                masterKey.fill(0)
            }
        } finally {
            derivedKey.fill(0)
        }
    }

    private fun parseEntries(entries: List<JsonObject>): List<OtpEntry> {
        return entries.mapNotNull { obj ->
            val type = obj["type"]?.jsonPrimitive?.content?.uppercase() ?: return@mapNotNull null
            val info = obj["info"]?.jsonObject ?: return@mapNotNull null

            val isSteam = type == "STEAM"
            val otpType = if (isSteam) "TOTP" else if (type == "TOTP" || type == "HOTP") type else return@mapNotNull null

            OtpEntry(
                type = otpType,
                label = obj["name"]?.jsonPrimitive?.content ?: "",
                issuer = if (isSteam) "Steam" else (obj["issuer"]?.jsonPrimitive?.content ?: ""),
                secret = info["secret"]?.jsonPrimitive?.content?.uppercase() ?: return@mapNotNull null,
                algo = info["algo"]?.jsonPrimitive?.content?.uppercase() ?: "SHA1",
                digits = info["digits"]?.jsonPrimitive?.int ?: 6,
                period = info["period"]?.jsonPrimitive?.int ?: 30,
                counter = info["counter"]?.jsonPrimitive?.long ?: 0L,
            )
        }
    }

    private fun exportPlain(path: String, entries: List<OtpEntry>) {
        val aegisEntries = entries.map { entry ->
            buildString {
                append("{")
                append("\"type\":\"${if (entry.issuer.equals("Steam", ignoreCase = true)) "steam" else entry.type.lowercase()}\",")
                append("\"name\":${jsonStr(entry.label)},")
                append("\"issuer\":${jsonStr(entry.issuer)},")
                append("\"icon\":null,\"icon_mime\":null,")
                append("\"info\":{")
                append("\"secret\":${jsonStr(entry.secret)},")
                append("\"digits\":${entry.digits},")
                append("\"algo\":${jsonStr(entry.algo)},")
                if (entry.type == "TOTP") {
                    append("\"period\":${entry.period}")
                } else {
                    append("\"counter\":${entry.counter}")
                }
                append("}}")
            }
        }

        val output = """{"version":1,"header":null,"db":{"version":2,"entries":[${aegisEntries.joinToString(",")}]}}"""
        File(path).writeText(output, Charsets.UTF_8)
    }

    private fun exportEncrypted(path: String, password: String, entries: List<OtpEntry>) {
        val random = SecureRandom()

        // Generate master key
        val masterKey = ByteArray(KEY_SIZE).also { random.nextBytes(it) }
        val salt = ByteArray(SALT_SIZE).also { random.nextBytes(it) }
        val keyNonce = ByteArray(NONCE_SIZE).also { random.nextBytes(it) }

        // Derive key with Scrypt (n=32768, r=8, p=1)
        val derivedKey = SCrypt.generate(
            password.toByteArray(Charsets.UTF_8),
            salt, 32768, 8, 1, KEY_SIZE,
        )

        try {
            // Encrypt master key
            val encMasterKey = AesGcmCipher.encrypt(
                plaintext = masterKey,
                key = derivedKey,
                iv = keyNonce,
                aad = ByteArray(0),
            )

            // Build database JSON
            val dbContent = buildDbJson(entries)
            val dbNonce = ByteArray(NONCE_SIZE).also { random.nextBytes(it) }
            val encDb = AesGcmCipher.encrypt(
                plaintext = dbContent.toByteArray(Charsets.UTF_8),
                key = masterKey,
                iv = dbNonce,
                aad = ByteArray(0),
            )

            val uuid = UUID.randomUUID().toString()

            // Aegis encrypted format: db field is base64(ciphertext), tag is separate in params
            val dbB64Ct = Base64.encodeToString(encDb.ciphertext, Base64.NO_WRAP)

            val output = buildString {
                append("{\"version\":1,\"header\":{")
                append("\"slots\":[{")
                append("\"type\":1,")
                append("\"uuid\":\"$uuid\",")
                append("\"key\":\"${HexUtils.bytesToHex(encMasterKey.ciphertext)}\",")
                append("\"key_params\":{")
                append("\"nonce\":\"${HexUtils.bytesToHex(keyNonce)}\",")
                append("\"tag\":\"${HexUtils.bytesToHex(encMasterKey.tag)}\"")
                append("},")
                append("\"n\":32768,\"r\":8,\"p\":1,")
                append("\"salt\":\"${HexUtils.bytesToHex(salt)}\"")
                append("}],")
                append("\"params\":{")
                append("\"nonce\":\"${HexUtils.bytesToHex(dbNonce)}\",")
                append("\"tag\":\"${HexUtils.bytesToHex(encDb.tag)}\"")
                append("}},")
                append("\"db\":\"$dbB64Ct\"")
                append("}")
            }
            File(path).writeText(output, Charsets.UTF_8)
        } finally {
            derivedKey.fill(0)
            masterKey.fill(0)
        }
    }

    private fun buildDbJson(entries: List<OtpEntry>): String {
        val aegisEntries = entries.map { entry ->
            buildString {
                append("{")
                append("\"type\":\"${if (entry.issuer.equals("Steam", ignoreCase = true)) "steam" else entry.type.lowercase()}\",")
                append("\"name\":${jsonStr(entry.label)},")
                append("\"issuer\":${jsonStr(entry.issuer)},")
                append("\"icon\":null,\"icon_mime\":null,")
                append("\"info\":{")
                append("\"secret\":${jsonStr(entry.secret)},")
                append("\"digits\":${entry.digits},")
                append("\"algo\":${jsonStr(entry.algo)},")
                if (entry.type == "TOTP") {
                    append("\"period\":${entry.period}")
                } else {
                    append("\"counter\":${entry.counter}")
                }
                append("}}")
            }
        }
        return """{"version":2,"entries":[${aegisEntries.joinToString(",")}]}"""
    }

    private fun jsonStr(s: String): String {
        return "\"${s.replace("\\", "\\\\").replace("\"", "\\\"")}\""
    }
}

class ImportExportException(message: String, cause: Throwable? = null) : Exception(message, cause)
