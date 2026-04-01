package com.otpclient.android.core.database

import com.otpclient.android.core.model.CryptoConstants
import java.nio.ByteBuffer
import java.nio.ByteOrder

data class DbHeaderV1(
    val iv: ByteArray,
    val salt: ByteArray,
) {
    fun toByteArray(): ByteArray {
        val buffer = ByteBuffer.allocate(CryptoConstants.DB_HEADER_V1_SIZE)
        buffer.put(iv)
        buffer.put(salt)
        return buffer.array()
    }

    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is DbHeaderV1) return false
        return iv.contentEquals(other.iv) && salt.contentEquals(other.salt)
    }

    override fun hashCode(): Int = 31 * iv.contentHashCode() + salt.contentHashCode()

    companion object {
        fun fromByteArray(data: ByteArray): DbHeaderV1 {
            val buffer = ByteBuffer.wrap(data)
            val iv = ByteArray(CryptoConstants.IV_SIZE)
            val salt = ByteArray(CryptoConstants.KDF_SALT_SIZE)
            buffer.get(iv)
            buffer.get(salt)
            return DbHeaderV1(iv = iv, salt = salt)
        }
    }
}

data class DbHeaderV2(
    val headerName: ByteArray = CryptoConstants.DB_HEADER_NAME.toByteArray(Charsets.US_ASCII),
    val dbVersion: Int = CryptoConstants.DB_VERSION,
    val iv: ByteArray,
    val salt: ByteArray,
    val argon2idIter: Int = CryptoConstants.ARGON2ID_DEFAULT_ITER,
    val argon2idMemcost: Int = CryptoConstants.ARGON2ID_DEFAULT_MC,
    val argon2idParallelism: Int = CryptoConstants.ARGON2ID_DEFAULT_PARAL,
) {
    /**
     * Serialize to the exact same byte layout as the C struct DbHeaderData_v2,
     * including the 3 padding bytes between header_name[9] and db_version (gint32).
     *
     * Layout (76 bytes total):
     *   offset  0: header_name[9]
     *   offset  9: padding[3]  (zeroed)
     *   offset 12: db_version  (int32 LE)
     *   offset 16: iv[16]
     *   offset 32: salt[32]
     *   offset 64: argon2id_iter (int32 LE)
     *   offset 68: argon2id_memcost (int32 LE)
     *   offset 72: argon2id_parallelism (int32 LE)
     */
    fun toByteArray(): ByteArray {
        val buffer = ByteBuffer.allocate(CryptoConstants.DB_HEADER_V2_SIZE)
        buffer.order(ByteOrder.LITTLE_ENDIAN)

        // header_name[9]
        buffer.put(headerName, 0, CryptoConstants.DB_HEADER_NAME_LEN)
        // 3 bytes padding (zeroed by allocate)
        buffer.position(buffer.position() + CryptoConstants.DB_HEADER_V2_PADDING)
        // db_version (int32 LE)
        buffer.putInt(dbVersion)
        // iv[16]
        buffer.put(iv)
        // salt[32]
        buffer.put(salt)
        // argon2id params (int32 LE each)
        buffer.putInt(argon2idIter)
        buffer.putInt(argon2idMemcost)
        buffer.putInt(argon2idParallelism)

        return buffer.array()
    }

    override fun equals(other: Any?): Boolean {
        if (this === other) return true
        if (other !is DbHeaderV2) return false
        return headerName.contentEquals(other.headerName) &&
            dbVersion == other.dbVersion &&
            iv.contentEquals(other.iv) &&
            salt.contentEquals(other.salt) &&
            argon2idIter == other.argon2idIter &&
            argon2idMemcost == other.argon2idMemcost &&
            argon2idParallelism == other.argon2idParallelism
    }

    override fun hashCode(): Int {
        var result = headerName.contentHashCode()
        result = 31 * result + dbVersion
        result = 31 * result + iv.contentHashCode()
        result = 31 * result + salt.contentHashCode()
        result = 31 * result + argon2idIter
        result = 31 * result + argon2idMemcost
        result = 31 * result + argon2idParallelism
        return result
    }

    companion object {
        fun fromByteArray(data: ByteArray): DbHeaderV2 {
            val buffer = ByteBuffer.wrap(data)
            buffer.order(ByteOrder.LITTLE_ENDIAN)

            val headerName = ByteArray(CryptoConstants.DB_HEADER_NAME_LEN)
            buffer.get(headerName)
            // skip 3 padding bytes
            buffer.position(buffer.position() + CryptoConstants.DB_HEADER_V2_PADDING)

            val dbVersion = buffer.getInt()
            val iv = ByteArray(CryptoConstants.IV_SIZE)
            buffer.get(iv)
            val salt = ByteArray(CryptoConstants.KDF_SALT_SIZE)
            buffer.get(salt)
            val argon2idIter = buffer.getInt()
            val argon2idMemcost = buffer.getInt()
            val argon2idParallelism = buffer.getInt()

            return DbHeaderV2(
                headerName = headerName,
                dbVersion = dbVersion,
                iv = iv,
                salt = salt,
                argon2idIter = argon2idIter,
                argon2idMemcost = argon2idMemcost,
                argon2idParallelism = argon2idParallelism,
            )
        }
    }
}
