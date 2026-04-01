package com.otpclient.android.core.otp

import java.nio.ByteBuffer
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec
import kotlin.math.pow

object OtpGenerator {

    private val STEAM_ALPHABET = "23456789BCDFGHJKMNPQRTVWXY".toCharArray()

    fun generateTotp(
        secret: String,
        period: Int = 30,
        digits: Int = 6,
        algorithm: String = "SHA1",
        timeSeconds: Long = System.currentTimeMillis() / 1000,
    ): String {
        val counter = timeSeconds / period
        return generateHotp(secret, counter, digits, algorithm)
    }

    fun generateHotp(
        secret: String,
        counter: Long,
        digits: Int = 6,
        algorithm: String = "SHA1",
    ): String {
        val key = Base32.decode(secret)
        val hmacAlgo = "Hmac$algorithm"
        val counterBytes = ByteBuffer.allocate(8).putLong(counter).array()

        val mac = Mac.getInstance(hmacAlgo)
        mac.init(SecretKeySpec(key, "RAW"))
        val hash = mac.doFinal(counterBytes)

        val offset = hash[hash.size - 1].toInt() and 0x0F
        val binary = ((hash[offset].toInt() and 0x7F) shl 24) or
            ((hash[offset + 1].toInt() and 0xFF) shl 16) or
            ((hash[offset + 2].toInt() and 0xFF) shl 8) or
            (hash[offset + 3].toInt() and 0xFF)

        val otp = binary % 10.0.pow(digits).toInt()
        return otp.toString().padStart(digits, '0')
    }

    fun generateSteam(
        secret: String,
        timeSeconds: Long = System.currentTimeMillis() / 1000,
    ): String {
        val key = Base32.decode(secret)
        val counter = timeSeconds / 30
        val counterBytes = ByteBuffer.allocate(8).putLong(counter).array()

        val mac = Mac.getInstance("HmacSHA1")
        mac.init(SecretKeySpec(key, "RAW"))
        val hash = mac.doFinal(counterBytes)

        val offset = hash[hash.size - 1].toInt() and 0x0F
        var code = ((hash[offset].toInt() and 0x7F) shl 24) or
            ((hash[offset + 1].toInt() and 0xFF) shl 16) or
            ((hash[offset + 2].toInt() and 0xFF) shl 8) or
            (hash[offset + 3].toInt() and 0xFF)

        val sb = StringBuilder()
        repeat(5) {
            sb.append(STEAM_ALPHABET[code % STEAM_ALPHABET.size])
            code /= STEAM_ALPHABET.size
        }
        return sb.toString()
    }

    fun remainingSeconds(period: Int = 30): Int {
        val now = System.currentTimeMillis() / 1000
        return (period - (now % period)).toInt()
    }
}
