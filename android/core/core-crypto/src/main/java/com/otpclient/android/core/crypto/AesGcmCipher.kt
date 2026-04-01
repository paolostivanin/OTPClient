package com.otpclient.android.core.crypto

import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

object AesGcmCipher {

    private const val TRANSFORMATION = "AES/GCM/NoPadding"
    private const val TAG_BIT_LENGTH = 128 // 16 bytes
    private const val KEY_ALGORITHM = "AES"

    fun encrypt(
        plaintext: ByteArray,
        key: ByteArray,
        iv: ByteArray,
        aad: ByteArray,
    ): EncryptedData {
        val cipher = Cipher.getInstance(TRANSFORMATION)
        val spec = GCMParameterSpec(TAG_BIT_LENGTH, iv)
        cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, KEY_ALGORITHM), spec)
        cipher.updateAAD(aad)

        // JCE appends the tag to the ciphertext
        val ciphertextWithTag = cipher.doFinal(plaintext)
        val tagOffset = ciphertextWithTag.size - 16
        val ciphertext = ciphertextWithTag.copyOfRange(0, tagOffset)
        val tag = ciphertextWithTag.copyOfRange(tagOffset, ciphertextWithTag.size)

        return EncryptedData(ciphertext = ciphertext, tag = tag)
    }

    fun decrypt(
        ciphertext: ByteArray,
        tag: ByteArray,
        key: ByteArray,
        iv: ByteArray,
        aad: ByteArray,
    ): ByteArray {
        val cipher = Cipher.getInstance(TRANSFORMATION)
        val spec = GCMParameterSpec(TAG_BIT_LENGTH, iv)
        cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, KEY_ALGORITHM), spec)
        cipher.updateAAD(aad)

        // JCE expects tag appended to ciphertext
        val combined = ciphertext + tag
        return cipher.doFinal(combined)
    }

    fun generateIv(size: Int = 16): ByteArray {
        val iv = ByteArray(size)
        SecureRandom().nextBytes(iv)
        return iv
    }

    fun generateSalt(size: Int = 32): ByteArray {
        val salt = ByteArray(size)
        SecureRandom().nextBytes(salt)
        return salt
    }
}

data class EncryptedData(
    val ciphertext: ByteArray,
    val tag: ByteArray,
) {
    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is EncryptedData) return false
        return ciphertext.contentEquals(other.ciphertext) && tag.contentEquals(other.tag)
    }

    override fun hashCode(): Int {
        return 31 * ciphertext.contentHashCode() + tag.contentHashCode()
    }
}
