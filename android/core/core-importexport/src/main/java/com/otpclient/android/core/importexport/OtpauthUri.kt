package com.otpclient.android.core.importexport

import android.net.Uri
import com.otpclient.android.core.model.OtpEntry

object OtpauthUri {

    fun parse(uriString: String): OtpEntry? {
        val cleaned = uriString.replace("%00", "").trim()
        if (!cleaned.startsWith("otpauth://")) return null

        val uri = Uri.parse(cleaned)
        val type = uri.host?.uppercase() ?: return null
        if (type != "TOTP" && type != "HOTP") return null

        val path = uri.path?.removePrefix("/") ?: ""
        val secret = uri.getQueryParameter("secret") ?: return null

        // Parse issuer and label from path: "Issuer:account" or just "account"
        var issuer = uri.getQueryParameter("issuer") ?: ""
        var label = path

        if (path.contains(":")) {
            val parts = path.split(":", limit = 2)
            if (issuer.isEmpty()) issuer = Uri.decode(parts[0])
            label = Uri.decode(parts[1])
        } else {
            label = Uri.decode(path)
        }

        val algorithm = uri.getQueryParameter("algorithm")?.uppercase() ?: "SHA1"
        val digits = uri.getQueryParameter("digits")?.toIntOrNull() ?: 6
        val period = uri.getQueryParameter("period")?.toIntOrNull() ?: 30
        val counter = uri.getQueryParameter("counter")?.toLongOrNull() ?: 0L

        // Steam detection
        val actualType = if (issuer.equals("Steam", ignoreCase = true)) "TOTP" else type

        return OtpEntry(
            type = actualType,
            label = label,
            issuer = issuer,
            secret = secret.uppercase(),
            algo = if (algorithm in listOf("SHA1", "SHA256", "SHA512")) algorithm else "SHA1",
            digits = if (digits in 6..8) digits else 6,
            period = period,
            counter = counter,
        )
    }

    fun toUri(entry: OtpEntry): String {
        val sb = StringBuilder("otpauth://")
        val isSteam = entry.issuer.equals("Steam", ignoreCase = true)

        if (isSteam) {
            sb.append("totp/")
            sb.append(Uri.encode("Steam:${entry.label}"))
        } else {
            sb.append(entry.type.lowercase())
            sb.append("/")
            val label = if (entry.issuer.isNotEmpty()) {
                "${entry.issuer}:${entry.label}"
            } else {
                entry.label
            }
            sb.append(Uri.encode(label))
        }

        sb.append("?secret=").append(entry.secret)

        if (entry.issuer.isNotEmpty()) {
            sb.append("&issuer=").append(Uri.encode(entry.issuer))
        }

        sb.append("&digits=").append(entry.digits)
        sb.append("&algorithm=").append(entry.algo)

        if (entry.type == "TOTP") {
            sb.append("&period=").append(entry.period)
        } else {
            sb.append("&counter=").append(entry.counter)
        }

        return sb.toString()
    }
}
