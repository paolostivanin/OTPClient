package com.otpclient.android.core.importexport

import android.util.Base64
import com.otpclient.android.core.crypto.AesGcmCipher
import com.otpclient.android.core.model.OtpEntry
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.long
import java.io.File
import java.security.SecureRandom
import javax.crypto.SecretKeyFactory
import javax.crypto.spec.PBEKeySpec

object TwoFasProvider {

    private const val KDF_ITERS = 10000
    private const val SALT_SIZE = 256
    private const val IV_SIZE = 12
    private const val TAG_SIZE = 16
    private const val KEY_SIZE = 32
    private const val SCHEMA_VERSION = 4

    private val json = Json { ignoreUnknownKeys = true }

    fun import(path: String, password: String?): List<OtpEntry> {
        val content = File(path).readText(Charsets.UTF_8)
        val root = json.parseToJsonElement(content).jsonObject

        val schemaVersion = root["schemaVersion"]?.jsonPrimitive?.int
        if (schemaVersion != SCHEMA_VERSION) {
            throw ImportExportException("Unsupported 2FAS schema version: $schemaVersion (expected $SCHEMA_VERSION)")
        }

        val encryptedField = root["servicesEncrypted"]?.jsonPrimitive?.content
        return if (encryptedField != null && encryptedField.isNotEmpty() && password != null) {
            importEncrypted(encryptedField, password)
        } else {
            val services = root["services"]?.jsonArray ?: throw ImportExportException("Missing 'services'")
            parseServices(services.map { it.jsonObject })
        }
    }

    fun export(path: String, password: String?, entries: List<OtpEntry>) {
        val epochTime = System.currentTimeMillis() / 1000
        if (password != null) {
            exportEncrypted(path, password, entries, epochTime)
        } else {
            exportPlain(path, entries, epochTime)
        }
    }

    private fun importEncrypted(encryptedField: String, password: String): List<OtpEntry> {
        // Format: base64(ciphertext+tag):base64(salt):base64(iv)
        val parts = encryptedField.split(":")
        if (parts.size != 3) throw ImportExportException("Invalid encrypted data format")

        val ciphertextWithTag = Base64.decode(parts[0], Base64.DEFAULT)
        val salt = Base64.decode(parts[1], Base64.DEFAULT)
        val iv = Base64.decode(parts[2], Base64.DEFAULT)

        val ciphertext = ciphertextWithTag.copyOfRange(0, ciphertextWithTag.size - TAG_SIZE)
        val tag = ciphertextWithTag.copyOfRange(ciphertextWithTag.size - TAG_SIZE, ciphertextWithTag.size)

        val key = deriveKey(password, salt)

        val decrypted = AesGcmCipher.decrypt(
            ciphertext = ciphertext,
            tag = tag,
            key = key,
            iv = iv,
            aad = ByteArray(0),
        )

        val jsonString = String(decrypted, Charsets.UTF_8)
        val services = json.parseToJsonElement(jsonString).jsonArray
        return parseServices(services.map { it.jsonObject })
    }

    private fun parseServices(services: List<kotlinx.serialization.json.JsonObject>): List<OtpEntry> {
        return services.mapNotNull { obj ->
            val otp = obj["otp"]?.jsonObject ?: return@mapNotNull null
            val tokenType = otp["tokenType"]?.jsonPrimitive?.content?.uppercase() ?: "TOTP"

            val isSteam = tokenType == "STEAM"
            val type = if (isSteam) "TOTP" else if (tokenType == "HOTP") "HOTP" else "TOTP"

            val secret = obj["secret"]?.jsonPrimitive?.content
                ?: otp["secret"]?.jsonPrimitive?.content
                ?: return@mapNotNull null

            OtpEntry(
                type = type,
                label = otp["account"]?.jsonPrimitive?.content
                    ?: otp["label"]?.jsonPrimitive?.content ?: "",
                issuer = if (isSteam) "Steam" else (otp["issuer"]?.jsonPrimitive?.content
                    ?: obj["name"]?.jsonPrimitive?.content ?: ""),
                secret = secret.uppercase(),
                algo = otp["algorithm"]?.jsonPrimitive?.content?.uppercase() ?: "SHA1",
                digits = otp["digits"]?.jsonPrimitive?.int ?: 6,
                period = otp["period"]?.jsonPrimitive?.int ?: 30,
                counter = otp["counter"]?.jsonPrimitive?.long ?: 0L,
            )
        }
    }

    private fun exportPlain(path: String, entries: List<OtpEntry>, epochTime: Long) {
        val services = buildServicesJson(entries, epochTime)
        val output = """{"services":[$services],"groups":[],"updatedAt":$epochTime,"schemaVersion":$SCHEMA_VERSION}"""
        File(path).writeText(output, Charsets.UTF_8)
    }

    private fun exportEncrypted(path: String, password: String, entries: List<OtpEntry>, epochTime: Long) {
        val random = SecureRandom()
        val servicesJson = "[${buildServicesJson(entries, epochTime)}]"

        val salt = ByteArray(SALT_SIZE).also { random.nextBytes(it) }
        val iv = ByteArray(IV_SIZE).also { random.nextBytes(it) }
        val key = deriveKey(password, salt)

        val encrypted = AesGcmCipher.encrypt(
            plaintext = servicesJson.toByteArray(Charsets.UTF_8),
            key = key,
            iv = iv,
            aad = ByteArray(0),
        )

        val ciphertextWithTag = encrypted.ciphertext + encrypted.tag
        val encodedData = "${Base64.encodeToString(ciphertextWithTag, Base64.NO_WRAP)}:${Base64.encodeToString(salt, Base64.NO_WRAP)}:${Base64.encodeToString(iv, Base64.NO_WRAP)}"

        // Reference data
        val refSalt = ByteArray(SALT_SIZE).also { random.nextBytes(it) }
        val refIv = ByteArray(IV_SIZE).also { random.nextBytes(it) }
        val refKey = deriveKey(password, refSalt)
        val refData = "2fas-browser-extension".toByteArray(Charsets.UTF_8)
        val refEncrypted = AesGcmCipher.encrypt(
            plaintext = refData,
            key = refKey,
            iv = refIv,
            aad = ByteArray(0),
        )
        val refCiphertextWithTag = refEncrypted.ciphertext + refEncrypted.tag
        val encodedRef = "${Base64.encodeToString(refCiphertextWithTag, Base64.NO_WRAP)}:${Base64.encodeToString(refSalt, Base64.NO_WRAP)}:${Base64.encodeToString(refIv, Base64.NO_WRAP)}"

        val output = buildString {
            append("{\"services\":[],\"groups\":[],\"schemaVersion\":$SCHEMA_VERSION,")
            append("\"servicesEncrypted\":\"$encodedData\",")
            append("\"reference\":\"$encodedRef\"}")
        }
        File(path).writeText(output, Charsets.UTF_8)
    }

    private fun buildServicesJson(entries: List<OtpEntry>, epochTime: Long): String {
        return entries.mapIndexed { index, entry ->
            val isSteam = entry.issuer.equals("Steam", ignoreCase = true)
            val tokenType = if (isSteam) "STEAM" else entry.type
            buildString {
                append("{")
                append("\"name\":${jsonStr(entry.issuer.ifEmpty { entry.label })},")
                append("\"secret\":${jsonStr(entry.secret)},")
                append("\"updatedAt\":$epochTime,")
                append("\"otp\":{")
                append("\"issuer\":${jsonStr(entry.issuer)},")
                append("\"account\":${jsonStr(entry.label)},")
                append("\"label\":${jsonStr(entry.label)},")
                append("\"algorithm\":${jsonStr(entry.algo)},")
                append("\"digits\":${entry.digits},")
                if (entry.type == "TOTP") {
                    append("\"period\":${entry.period},")
                } else {
                    append("\"counter\":${entry.counter},")
                }
                append("\"tokenType\":${jsonStr(tokenType)},")
                append("\"source\":\"Manual\"")
                append("},")
                append("\"order\":{\"position\":$index}")
                append("}")
            }
        }.joinToString(",")
    }

    private fun deriveKey(password: String, salt: ByteArray): ByteArray {
        val spec = PBEKeySpec(password.toCharArray(), salt, KDF_ITERS, KEY_SIZE * 8)
        val factory = SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256")
        return factory.generateSecret(spec).encoded
    }

    private fun jsonStr(s: String): String {
        return "\"${s.replace("\\", "\\\\").replace("\"", "\\\"")}\""
    }
}
