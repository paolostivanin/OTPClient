package com.otpclient.android.core.importexport

import com.otpclient.android.core.model.OtpEntry
import java.io.File

object FreeOtpPlusProvider {

    fun import(path: String): List<OtpEntry> {
        val content = File(path).readText(Charsets.UTF_8)
        return content.lines()
            .filter { it.contains("otpauth://") }
            .mapNotNull { line ->
                val start = line.indexOf("otpauth://")
                OtpauthUri.parse(line.substring(start))
            }
    }

    fun export(path: String, entries: List<OtpEntry>) {
        val content = entries.joinToString("") { entry ->
            OtpauthUri.toUri(entry) + "\n"
        }
        File(path).writeText(content, Charsets.UTF_8)
    }
}
