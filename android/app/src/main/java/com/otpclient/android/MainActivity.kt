package com.otpclient.android

import android.os.Bundle
import android.view.WindowManager
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.otpclient.android.core.database.DatabaseRepository
import com.otpclient.android.feature.settings.AppSettings
import com.otpclient.android.feature.settings.SettingsRepository
import com.otpclient.android.feature.settings.Theme
import com.otpclient.android.lifecycle.AppLockManager
import com.otpclient.android.ui.OTPClientApp
import com.otpclient.android.ui.theme.OTPClientTheme
import com.otpclient.android.ui.theme.ThemeMode
import dagger.hilt.android.AndroidEntryPoint
import javax.inject.Inject

@AndroidEntryPoint
class MainActivity : ComponentActivity() {

    @Inject
    lateinit var settingsRepository: SettingsRepository

    @Inject
    lateinit var appLockManager: AppLockManager

    @Inject
    lateinit var databaseRepository: DatabaseRepository

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.setFlags(
            WindowManager.LayoutParams.FLAG_SECURE,
            WindowManager.LayoutParams.FLAG_SECURE,
        )
        enableEdgeToEdge()
        setContent {
            val settings by settingsRepository.settings.collectAsStateWithLifecycle(
                initialValue = AppSettings(),
            )
            val themeMode = when (settings.theme) {
                Theme.LIGHT -> ThemeMode.LIGHT
                Theme.DARK -> ThemeMode.DARK
                Theme.SYSTEM -> ThemeMode.SYSTEM
            }
            OTPClientTheme(themeMode = themeMode) {
                Surface(
                    modifier = Modifier.fillMaxSize(),
                    color = MaterialTheme.colorScheme.background,
                ) {
                    OTPClientApp(
                        defaultDbPath = filesDir.resolve("otpclient.enc").absolutePath,
                        appLockManager = appLockManager,
                        databaseRepository = databaseRepository,
                    )
                }
            }
        }
    }
}
