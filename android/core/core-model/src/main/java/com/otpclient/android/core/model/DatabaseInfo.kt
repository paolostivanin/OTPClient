package com.otpclient.android.core.model

data class DatabaseInfo(
    val name: String,
    val path: String,
    val version: Int = 2,
    val argon2Params: Argon2Params = Argon2Params(),
)

data class Argon2Params(
    val iterations: Int = 4,
    val memoryCostKiB: Int = 131072,
    val parallelism: Int = 4,
)

data class DatabaseEntry(
    val name: String,
    val path: String,
)
