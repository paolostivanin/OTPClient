package com.otpclient.android.feature.settings

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import javax.inject.Inject
import javax.inject.Singleton

private val Context.dataStore: DataStore<Preferences> by preferencesDataStore(name = "settings")

@Singleton
class SettingsRepository @Inject constructor(
    @ApplicationContext private val context: Context,
) {
    private object Keys {
        val THEME = stringPreferencesKey("theme")
        val BIOMETRIC_ENABLED = booleanPreferencesKey("biometric_enabled")
        val AUTO_LOCK_SECONDS = intPreferencesKey("auto_lock_seconds")
        val SHOW_NEXT_OTP = booleanPreferencesKey("show_next_otp")
    }

    val settings: Flow<AppSettings> = context.dataStore.data.map { prefs ->
        AppSettings(
            theme = Theme.fromString(prefs[Keys.THEME]),
            biometricEnabled = prefs[Keys.BIOMETRIC_ENABLED] ?: false,
            autoLockSeconds = prefs[Keys.AUTO_LOCK_SECONDS] ?: 0,
            showNextOtp = prefs[Keys.SHOW_NEXT_OTP] ?: false,
        )
    }

    suspend fun setTheme(theme: Theme) {
        context.dataStore.edit { it[Keys.THEME] = theme.name }
    }

    suspend fun setBiometricEnabled(enabled: Boolean) {
        context.dataStore.edit { it[Keys.BIOMETRIC_ENABLED] = enabled }
    }

    suspend fun setAutoLockSeconds(seconds: Int) {
        context.dataStore.edit { it[Keys.AUTO_LOCK_SECONDS] = seconds }
    }

    suspend fun setShowNextOtp(show: Boolean) {
        context.dataStore.edit { it[Keys.SHOW_NEXT_OTP] = show }
    }
}

data class AppSettings(
    val theme: Theme = Theme.SYSTEM,
    val biometricEnabled: Boolean = false,
    val autoLockSeconds: Int = 0,
    val showNextOtp: Boolean = false,
)

enum class Theme {
    SYSTEM, LIGHT, DARK;

    companion object {
        fun fromString(value: String?): Theme = when (value) {
            "LIGHT" -> LIGHT
            "DARK" -> DARK
            else -> SYSTEM
        }
    }
}
