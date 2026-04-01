package com.otpclient.android.core.otp

object Base32 {
    private const val ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567"

    fun decode(input: String): ByteArray {
        val cleaned = input.uppercase().replace("=", "").replace(" ", "")
        if (cleaned.isEmpty()) return ByteArray(0)

        val output = ByteArray(cleaned.length * 5 / 8)
        var buffer = 0
        var bitsLeft = 0
        var index = 0

        for (c in cleaned) {
            val value = ALPHABET.indexOf(c)
            if (value < 0) throw IllegalArgumentException("Invalid Base32 character: $c")

            buffer = (buffer shl 5) or value
            bitsLeft += 5

            if (bitsLeft >= 8) {
                bitsLeft -= 8
                output[index++] = (buffer shr bitsLeft).toByte()
            }
        }

        return output.copyOf(index)
    }

    fun encode(input: ByteArray): String {
        if (input.isEmpty()) return ""

        val sb = StringBuilder()
        var buffer = 0
        var bitsLeft = 0

        for (b in input) {
            buffer = (buffer shl 8) or (b.toInt() and 0xFF)
            bitsLeft += 8

            while (bitsLeft >= 5) {
                bitsLeft -= 5
                sb.append(ALPHABET[(buffer shr bitsLeft) and 0x1F])
            }
        }

        if (bitsLeft > 0) {
            sb.append(ALPHABET[(buffer shl (5 - bitsLeft)) and 0x1F])
        }

        return sb.toString()
    }
}
