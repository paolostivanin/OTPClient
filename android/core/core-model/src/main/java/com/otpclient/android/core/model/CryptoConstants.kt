package com.otpclient.android.core.model

object CryptoConstants {
    const val IV_SIZE = 16
    const val KDF_SALT_SIZE = 32
    const val TAG_SIZE = 16
    const val KEY_SIZE = 32

    const val DB_HEADER_NAME = "OTPClient"
    const val DB_HEADER_NAME_LEN = 9
    const val DB_VERSION = 2

    // V2 header: 9 (name) + 3 (padding) + 4 (version) + 16 (iv) + 32 (salt) + 4 + 4 + 4 = 76
    const val DB_HEADER_V2_SIZE = 76
    const val DB_HEADER_V2_PADDING = 3

    // V1 header: 16 (iv) + 32 (salt) = 48
    const val DB_HEADER_V1_SIZE = 48

    // Argon2id defaults
    const val ARGON2ID_TAGLEN = 32
    const val ARGON2ID_KEYLEN = 32
    const val ARGON2ID_DEFAULT_ITER = 4
    const val ARGON2ID_DEFAULT_MC = 131072 // 128 MiB
    const val ARGON2ID_DEFAULT_PARAL = 4

    // PBKDF2 (v1 legacy)
    const val KDF_ITERATIONS = 100_000
}
