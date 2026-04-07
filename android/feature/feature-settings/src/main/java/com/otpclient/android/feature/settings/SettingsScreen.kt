package com.otpclient.android.feature.settings

import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Cloud
import androidx.compose.material.icons.filled.DarkMode
import androidx.compose.material.icons.filled.Fingerprint
import androidx.compose.material.icons.filled.LockClock
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material3.Button
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.otpclient.android.core.sync.SyncProviderType
import com.otpclient.android.core.sync.WebDavConfig

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun SettingsScreen(
    onBack: () -> Unit,
    viewModel: SettingsViewModel = hiltViewModel(),
) {
    val settings by viewModel.settings.collectAsStateWithLifecycle()
    val syncState by viewModel.syncState.collectAsStateWithLifecycle()
    val webDavTestResult by viewModel.webDavTestResult.collectAsStateWithLifecycle()

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.settings_title)) },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = stringResource(R.string.content_desc_back))
                    }
                },
            )
        },
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(horizontal = 16.dp)
                .verticalScroll(rememberScrollState()),
        ) {
            SectionHeader(stringResource(R.string.settings_section_appearance))

            ThemeSelector(
                currentTheme = settings.theme,
                onThemeSelected = viewModel::setTheme,
            )

            Spacer(modifier = Modifier.height(16.dp))

            SectionHeader(stringResource(R.string.settings_section_security))

            SettingSwitch(
                icon = Icons.Default.Fingerprint,
                title = stringResource(R.string.settings_biometric_title),
                subtitle = stringResource(R.string.settings_biometric_subtitle),
                checked = settings.biometricEnabled,
                onCheckedChange = viewModel::setBiometricEnabled,
            )

            Spacer(modifier = Modifier.height(8.dp))

            AutoLockSelector(
                currentSeconds = settings.autoLockSeconds,
                onSelected = viewModel::setAutoLockSeconds,
            )

            Spacer(modifier = Modifier.height(16.dp))

            SectionHeader(stringResource(R.string.settings_section_display))

            SettingSwitch(
                icon = Icons.Default.Visibility,
                title = stringResource(R.string.settings_show_next_otp_title),
                subtitle = stringResource(R.string.settings_show_next_otp_subtitle),
                checked = settings.showNextOtp,
                onCheckedChange = viewModel::setShowNextOtp,
            )

            Spacer(modifier = Modifier.height(16.dp))

            SectionHeader(stringResource(R.string.settings_section_sync))

            SyncProviderSelector(
                currentType = syncState.providerType,
                onTypeSelected = viewModel::setSyncProviderType,
            )

            when (syncState.providerType) {
                SyncProviderType.GOOGLE_DRIVE -> {
                    Spacer(modifier = Modifier.height(8.dp))
                    GoogleDriveSettings(
                        accountEmail = syncState.googleAccountEmail,
                        onSignIn = { /* Google Sign-In intent handled by Activity */ },
                        onSignOut = { viewModel.setGoogleAccountEmail(null) },
                    )
                }
                SyncProviderType.WEBDAV -> {
                    Spacer(modifier = Modifier.height(8.dp))
                    WebDavSettings(
                        config = syncState.webDavConfig,
                        testResult = webDavTestResult,
                        onSave = viewModel::saveWebDavConfig,
                        onTest = viewModel::testWebDavConnection,
                        onClearTestResult = viewModel::clearWebDavTestResult,
                    )
                }
                SyncProviderType.NONE -> {}
            }

            Spacer(modifier = Modifier.height(16.dp))
        }
    }
}

@Composable
private fun SectionHeader(title: String) {
    Text(
        text = title,
        style = MaterialTheme.typography.titleSmall,
        color = MaterialTheme.colorScheme.primary,
        modifier = Modifier.padding(vertical = 8.dp),
    )
}

@Composable
private fun SettingSwitch(
    icon: ImageVector,
    title: String,
    subtitle: String,
    checked: Boolean,
    onCheckedChange: (Boolean) -> Unit,
) {
    Card(
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainerLow,
        ),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .clickable { onCheckedChange(!checked) }
                .padding(16.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(icon, contentDescription = title, tint = MaterialTheme.colorScheme.onSurfaceVariant)
            Spacer(modifier = Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(title, style = MaterialTheme.typography.bodyLarge)
                Text(
                    subtitle,
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Switch(checked = checked, onCheckedChange = onCheckedChange)
        }
    }
}

@Composable
private fun ThemeSelector(
    currentTheme: Theme,
    onThemeSelected: (Theme) -> Unit,
) {
    Card(
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainerLow,
        ),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(
                Icons.Default.DarkMode,
                contentDescription = stringResource(R.string.settings_theme),
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(modifier = Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(stringResource(R.string.settings_theme), style = MaterialTheme.typography.bodyLarge)
                Spacer(modifier = Modifier.height(8.dp))
                Theme.entries.forEach { theme ->
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clickable { onThemeSelected(theme) }
                            .padding(vertical = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        RadioButton(
                            selected = currentTheme == theme,
                            onClick = { onThemeSelected(theme) },
                        )
                        Spacer(modifier = Modifier.width(8.dp))
                        Text(
                            text = when (theme) {
                                Theme.SYSTEM -> stringResource(R.string.settings_theme_system)
                                Theme.LIGHT -> stringResource(R.string.settings_theme_light)
                                Theme.DARK -> stringResource(R.string.settings_theme_dark)
                            },
                            style = MaterialTheme.typography.bodyMedium,
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun AutoLockSelector(
    currentSeconds: Int,
    onSelected: (Int) -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }
    val options = listOf(
        0 to stringResource(R.string.settings_autolock_disabled),
        30 to stringResource(R.string.settings_autolock_30s),
        60 to stringResource(R.string.settings_autolock_1m),
        120 to stringResource(R.string.settings_autolock_2m),
        300 to stringResource(R.string.settings_autolock_5m),
        600 to stringResource(R.string.settings_autolock_10m),
    )
    val currentLabel = options.firstOrNull { it.first == currentSeconds }?.second
        ?: stringResource(R.string.settings_autolock_disabled)

    Card(
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainerLow,
        ),
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .clickable { expanded = !expanded }
                .padding(16.dp),
        ) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Icon(
                    Icons.Default.LockClock,
                    contentDescription = stringResource(R.string.settings_autolock_title),
                    tint = MaterialTheme.colorScheme.onSurfaceVariant,
                )
                Spacer(modifier = Modifier.width(12.dp))
                Column(modifier = Modifier.weight(1f)) {
                    Text(stringResource(R.string.settings_autolock_title), style = MaterialTheme.typography.bodyLarge)
                    Text(
                        currentLabel,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }

            if (expanded) {
                Spacer(modifier = Modifier.height(8.dp))
                options.forEach { (seconds, label) ->
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clickable {
                                onSelected(seconds)
                                expanded = false
                            }
                            .padding(vertical = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        RadioButton(
                            selected = currentSeconds == seconds,
                            onClick = {
                                onSelected(seconds)
                                expanded = false
                            },
                        )
                        Spacer(modifier = Modifier.width(8.dp))
                        Text(label, style = MaterialTheme.typography.bodyMedium)
                    }
                }
            }
        }
    }
}

@Composable
private fun SyncProviderSelector(
    currentType: SyncProviderType,
    onTypeSelected: (SyncProviderType) -> Unit,
) {
    Card(
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainerLow,
        ),
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Icon(
                Icons.Default.Cloud,
                contentDescription = stringResource(R.string.settings_sync_provider),
                tint = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(modifier = Modifier.width(12.dp))
            Column(modifier = Modifier.weight(1f)) {
                Text(stringResource(R.string.settings_sync_provider), style = MaterialTheme.typography.bodyLarge)
                Spacer(modifier = Modifier.height(8.dp))
                val options = listOf(
                    SyncProviderType.NONE to stringResource(R.string.settings_sync_none),
                    SyncProviderType.GOOGLE_DRIVE to stringResource(R.string.settings_sync_google_drive),
                    SyncProviderType.WEBDAV to stringResource(R.string.settings_sync_webdav),
                )
                options.forEach { (type, label) ->
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .clickable { onTypeSelected(type) }
                            .padding(vertical = 4.dp),
                        verticalAlignment = Alignment.CenterVertically,
                    ) {
                        RadioButton(
                            selected = currentType == type,
                            onClick = { onTypeSelected(type) },
                        )
                        Spacer(modifier = Modifier.width(8.dp))
                        Text(label, style = MaterialTheme.typography.bodyMedium)
                    }
                }
            }
        }
    }
}

@Composable
private fun GoogleDriveSettings(
    accountEmail: String?,
    onSignIn: () -> Unit,
    onSignOut: () -> Unit,
) {
    Card(
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainerLow,
        ),
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
        ) {
            if (accountEmail != null) {
                Text(
                    text = stringResource(R.string.settings_sync_google_signed_in, accountEmail),
                    style = MaterialTheme.typography.bodyMedium,
                )
                Spacer(modifier = Modifier.height(8.dp))
                OutlinedButton(onClick = onSignOut, modifier = Modifier.fillMaxWidth()) {
                    Text(stringResource(R.string.settings_sync_google_sign_out))
                }
            } else {
                Button(onClick = onSignIn, modifier = Modifier.fillMaxWidth()) {
                    Text(stringResource(R.string.settings_sync_google_sign_in))
                }
            }
        }
    }
}

@Composable
private fun WebDavSettings(
    config: WebDavConfig,
    testResult: Boolean?,
    onSave: (WebDavConfig) -> Unit,
    onTest: (WebDavConfig) -> Unit,
    onClearTestResult: () -> Unit,
) {
    var serverUrl by remember(config) { mutableStateOf(config.serverUrl) }
    var username by remember(config) { mutableStateOf(config.username) }
    var password by remember(config) { mutableStateOf(config.password) }
    var remotePath by remember(config) { mutableStateOf(config.remotePath) }

    Card(
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.surfaceContainerLow,
        ),
    ) {
        Column(
            modifier = Modifier
                .fillMaxWidth()
                .padding(16.dp),
        ) {
            OutlinedTextField(
                value = serverUrl,
                onValueChange = { serverUrl = it; onClearTestResult() },
                label = { Text(stringResource(R.string.settings_sync_webdav_server_url)) },
                placeholder = { Text(stringResource(R.string.settings_sync_webdav_server_url_hint)) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(modifier = Modifier.height(8.dp))
            OutlinedTextField(
                value = username,
                onValueChange = { username = it; onClearTestResult() },
                label = { Text(stringResource(R.string.settings_sync_webdav_username)) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(modifier = Modifier.height(8.dp))
            OutlinedTextField(
                value = password,
                onValueChange = { password = it; onClearTestResult() },
                label = { Text(stringResource(R.string.settings_sync_webdav_password)) },
                singleLine = true,
                visualTransformation = PasswordVisualTransformation(),
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(modifier = Modifier.height(8.dp))
            OutlinedTextField(
                value = remotePath,
                onValueChange = { remotePath = it; onClearTestResult() },
                label = { Text(stringResource(R.string.settings_sync_webdav_remote_path)) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )
            Spacer(modifier = Modifier.height(12.dp))

            Row(modifier = Modifier.fillMaxWidth()) {
                OutlinedButton(
                    onClick = {
                        onTest(WebDavConfig(serverUrl, username, password, remotePath))
                    },
                    modifier = Modifier.weight(1f),
                ) {
                    Text(stringResource(R.string.settings_sync_webdav_test))
                }
                Spacer(modifier = Modifier.width(8.dp))
                Button(
                    onClick = {
                        onSave(WebDavConfig(serverUrl, username, password, remotePath))
                    },
                    modifier = Modifier.weight(1f),
                ) {
                    Text(stringResource(R.string.settings_sync_webdav_save))
                }
            }

            testResult?.let { success ->
                Spacer(modifier = Modifier.height(8.dp))
                Text(
                    text = stringResource(
                        if (success) R.string.settings_sync_webdav_test_success
                        else R.string.settings_sync_webdav_test_failed,
                    ),
                    style = MaterialTheme.typography.bodySmall,
                    color = if (success) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.error,
                )
            }
        }
    }
}
