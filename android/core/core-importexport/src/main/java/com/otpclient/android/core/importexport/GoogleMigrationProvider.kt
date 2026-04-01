package com.otpclient.android.core.importexport

import android.net.Uri
import android.util.Base64
import com.otpclient.android.core.model.OtpEntry

object GoogleMigrationProvider {

    fun parseFromUri(uriString: String): List<OtpEntry> {
        val uri = Uri.parse(uriString)
        if (uri.scheme != "otpauth-migration" || uri.host != "offline") {
            throw ImportExportException("Invalid Google migration URI")
        }

        val data = uri.getQueryParameter("data")
            ?: throw ImportExportException("Missing 'data' parameter")

        val decoded = Base64.decode(data, Base64.DEFAULT)
        return parseProtobuf(decoded)
    }

    private fun parseProtobuf(data: ByteArray): List<OtpEntry> {
        // Manual protobuf parsing for MigrationPayload
        // Field 1: repeated OtpParameters otp_parameters
        val entries = mutableListOf<OtpEntry>()
        var offset = 0

        while (offset < data.size) {
            val (fieldTag, newOffset) = readVarint(data, offset)
            offset = newOffset
            val fieldNumber = (fieldTag shr 3).toInt()
            val wireType = (fieldTag and 0x7).toInt()

            if (fieldNumber == 1 && wireType == 2) {
                // Length-delimited: OtpParameters message
                val (length, dataOffset) = readVarint(data, offset)
                offset = dataOffset
                val messageBytes = data.copyOfRange(offset, offset + length.toInt())
                offset += length.toInt()

                val entry = parseOtpParameters(messageBytes)
                if (entry != null) entries.add(entry)
            } else {
                // Skip unknown fields
                offset = skipField(data, offset, wireType)
            }
        }

        return entries
    }

    private fun parseOtpParameters(data: ByteArray): OtpEntry? {
        var secret: ByteArray? = null
        var name = ""
        var issuer = ""
        var algorithm = "SHA1"
        var digits = 6
        var type = "TOTP"
        var counter = 0L

        var offset = 0
        while (offset < data.size) {
            val (fieldTag, newOffset) = readVarint(data, offset)
            offset = newOffset
            val fieldNumber = (fieldTag shr 3).toInt()
            val wireType = (fieldTag and 0x7).toInt()

            when (fieldNumber) {
                1 -> { // secret (bytes)
                    val (length, dataOffset) = readVarint(data, offset)
                    offset = dataOffset
                    secret = data.copyOfRange(offset, offset + length.toInt())
                    offset += length.toInt()
                }
                2 -> { // name (string)
                    val (length, dataOffset) = readVarint(data, offset)
                    offset = dataOffset
                    name = String(data, offset, length.toInt(), Charsets.UTF_8)
                    offset += length.toInt()
                }
                3 -> { // issuer (string)
                    val (length, dataOffset) = readVarint(data, offset)
                    offset = dataOffset
                    issuer = String(data, offset, length.toInt(), Charsets.UTF_8)
                    offset += length.toInt()
                }
                4 -> { // algorithm (enum)
                    val (value, dataOffset) = readVarint(data, offset)
                    offset = dataOffset
                    algorithm = when (value.toInt()) {
                        1 -> "SHA1"
                        2 -> "SHA256"
                        3 -> "SHA512"
                        else -> "SHA1"
                    }
                }
                5 -> { // digits (enum)
                    val (value, dataOffset) = readVarint(data, offset)
                    offset = dataOffset
                    digits = when (value.toInt()) {
                        1 -> 6
                        2 -> 8
                        else -> 6
                    }
                }
                6 -> { // type (enum)
                    val (value, dataOffset) = readVarint(data, offset)
                    offset = dataOffset
                    type = when (value.toInt()) {
                        1 -> "HOTP"
                        2 -> "TOTP"
                        else -> "TOTP"
                    }
                }
                7 -> { // counter (int64)
                    val (value, dataOffset) = readVarint(data, offset)
                    offset = dataOffset
                    counter = value
                }
                else -> {
                    offset = skipField(data, offset, wireType)
                }
            }
        }

        if (secret == null) return null

        // Convert raw bytes to base32
        val base32Secret = com.otpclient.android.core.otp.Base32.encode(secret)

        // Parse issuer from name if format is "Issuer:Account"
        var parsedIssuer = issuer
        var parsedLabel = name
        if (name.contains(":") && issuer.isEmpty()) {
            val parts = name.split(":", limit = 2)
            parsedIssuer = parts[0].trim()
            parsedLabel = parts[1].trim()
        }

        return OtpEntry(
            type = type,
            label = parsedLabel,
            issuer = parsedIssuer,
            secret = base32Secret,
            algo = algorithm,
            digits = digits,
            period = 30,
            counter = counter,
        )
    }

    private fun readVarint(data: ByteArray, startOffset: Int): Pair<Long, Int> {
        var result = 0L
        var shift = 0
        var offset = startOffset
        while (offset < data.size) {
            val b = data[offset].toInt() and 0xFF
            offset++
            result = result or ((b.toLong() and 0x7F) shl shift)
            if (b and 0x80 == 0) break
            shift += 7
        }
        return Pair(result, offset)
    }

    private fun skipField(data: ByteArray, offset: Int, wireType: Int): Int {
        return when (wireType) {
            0 -> readVarint(data, offset).second // varint
            1 -> offset + 8 // 64-bit
            2 -> { // length-delimited
                val (length, newOffset) = readVarint(data, offset)
                newOffset + length.toInt()
            }
            5 -> offset + 4 // 32-bit
            else -> data.size // unknown, skip to end
        }
    }
}
