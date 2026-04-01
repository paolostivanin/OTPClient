package com.otpclient.android.core.model

import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable

@Serializable
data class OtpEntry(
    val type: String,
    val label: String,
    val issuer: String,
    val digits: Int = 6,
    val algo: String = "SHA1",
    val secret: String,
    val period: Int = 30,
    val counter: Long = 0,
)

enum class OtpType(val value: String) {
    TOTP("TOTP"),
    HOTP("HOTP");

    companion object {
        fun fromString(value: String): OtpType = when (value.uppercase()) {
            "TOTP" -> TOTP
            "HOTP" -> HOTP
            else -> TOTP
        }
    }
}

enum class Algorithm(val value: String) {
    SHA1("SHA1"),
    SHA256("SHA256"),
    SHA512("SHA512");

    val hmacName: String
        get() = "Hmac$value"

    companion object {
        fun fromString(value: String): Algorithm = when (value.uppercase()) {
            "SHA256" -> SHA256
            "SHA512" -> SHA512
            else -> SHA1
        }
    }
}
