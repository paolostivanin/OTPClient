package com.otpclient.android.core.crypto

import android.content.Context
import com.lambdapioneer.argon2kt.Argon2Kt
import com.lambdapioneer.argon2kt.Argon2KtResult
import com.lambdapioneer.argon2kt.Argon2Mode
import javax.crypto.SecretKeyFactory
import javax.crypto.spec.PBEKeySpec

object KeyDerivation {

    fun deriveKeyArgon2id(
        password: String,
        salt: ByteArray,
        iterations: Int,
        memoryCostKiB: Int,
        parallelism: Int,
        hashLength: Int = 32,
    ): ByteArray {
        val argon2 = Argon2Kt()
        val result: Argon2KtResult = argon2.hash(
            mode = Argon2Mode.ARGON2_ID,
            password = password.toByteArray(Charsets.UTF_8),
            salt = salt,
            tCostInIterations = iterations,
            mCostInKibibyte = memoryCostKiB,
            parallelism = parallelism,
            hashLengthInBytes = hashLength,
        )
        return result.rawHashAsByteArray()
    }

    fun deriveKeyPbkdf2Sha512(
        password: String,
        salt: ByteArray,
        iterations: Int = 100_000,
        keyLengthBits: Int = 256,
    ): ByteArray {
        val spec = PBEKeySpec(
            password.toCharArray(),
            salt,
            iterations,
            keyLengthBits,
        )
        val factory = SecretKeyFactory.getInstance("PBKDF2WithHmacSHA512")
        return factory.generateSecret(spec).encoded
    }
}
