package com.otpclient.android.feature.unlock

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Fingerprint
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Visibility
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.material3.Button
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.fragment.app.FragmentActivity
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle

@Composable
fun UnlockScreen(
    defaultDbPath: String,
    onUnlocked: () -> Unit,
    viewModel: UnlockViewModel = hiltViewModel(),
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val snackbarHostState = remember { SnackbarHostState() }
    val context = LocalContext.current
    val activity = context as? FragmentActivity

    var password by rememberSaveable { mutableStateOf("") }
    var confirmPassword by rememberSaveable { mutableStateOf("") }
    var passwordVisible by rememberSaveable { mutableStateOf(false) }
    val isNewDatabase = !viewModel.databaseExists(defaultDbPath)
    var showConfirmField by rememberSaveable { mutableStateOf(isNewDatabase) }

    val canBiometric = !isNewDatabase &&
        viewModel.canUseBiometric() &&
        viewModel.hasBiometricPassword(defaultDbPath)

    LaunchedEffect(canBiometric) {
        if (canBiometric && activity != null) {
            viewModel.unlockWithBiometric(activity, defaultDbPath)
        }
    }

    LaunchedEffect(uiState) {
        when (val state = uiState) {
            is UnlockUiState.Success -> onUnlocked()
            is UnlockUiState.Error -> snackbarHostState.showSnackbar(state.message)
            else -> {}
        }
    }

    Scaffold(
        snackbarHost = { SnackbarHost(snackbarHostState) },
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(horizontal = 32.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center,
        ) {
            Icon(
                imageVector = Icons.Default.Lock,
                contentDescription = stringResource(R.string.unlock_title),
                modifier = Modifier.size(64.dp),
                tint = MaterialTheme.colorScheme.primary,
            )

            Spacer(modifier = Modifier.height(16.dp))

            Text(
                text = stringResource(R.string.unlock_title),
                style = MaterialTheme.typography.headlineLarge,
                color = MaterialTheme.colorScheme.onSurface,
            )

            Spacer(modifier = Modifier.height(8.dp))

            Text(
                text = stringResource(
                    if (isNewDatabase) R.string.unlock_subtitle_new else R.string.unlock_subtitle_existing,
                ),
                style = MaterialTheme.typography.bodyLarge,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )

            Spacer(modifier = Modifier.height(32.dp))

            OutlinedTextField(
                value = password,
                onValueChange = { password = it },
                label = { Text(stringResource(R.string.unlock_password_label)) },
                singleLine = true,
                visualTransformation = if (passwordVisible) VisualTransformation.None else PasswordVisualTransformation(),
                keyboardOptions = KeyboardOptions(
                    keyboardType = KeyboardType.Password,
                    imeAction = if (showConfirmField) ImeAction.Next else ImeAction.Done,
                ),
                keyboardActions = KeyboardActions(
                    onDone = {
                        if (!showConfirmField && password.isNotEmpty()) {
                            viewModel.unlock(defaultDbPath, password)
                        }
                    },
                ),
                trailingIcon = {
                    IconButton(onClick = { passwordVisible = !passwordVisible }) {
                        Icon(
                            imageVector = if (passwordVisible) Icons.Default.VisibilityOff else Icons.Default.Visibility,
                            contentDescription = stringResource(
                                if (passwordVisible) R.string.unlock_hide_password else R.string.unlock_show_password,
                            ),
                        )
                    }
                },
                modifier = Modifier.fillMaxWidth(),
                enabled = uiState !is UnlockUiState.Loading,
            )

            AnimatedVisibility(visible = showConfirmField) {
                Column {
                    Spacer(modifier = Modifier.height(12.dp))
                    OutlinedTextField(
                        value = confirmPassword,
                        onValueChange = { confirmPassword = it },
                        label = { Text(stringResource(R.string.unlock_confirm_password_label)) },
                        singleLine = true,
                        visualTransformation = if (passwordVisible) VisualTransformation.None else PasswordVisualTransformation(),
                        keyboardOptions = KeyboardOptions(
                            keyboardType = KeyboardType.Password,
                            imeAction = ImeAction.Done,
                        ),
                        keyboardActions = KeyboardActions(
                            onDone = {
                                if (password.isNotEmpty() && password == confirmPassword) {
                                    viewModel.createDatabase(defaultDbPath, password)
                                }
                            },
                        ),
                        modifier = Modifier.fillMaxWidth(),
                        enabled = uiState !is UnlockUiState.Loading,
                        isError = confirmPassword.isNotEmpty() && password != confirmPassword,
                        supportingText = {
                            if (confirmPassword.isNotEmpty() && password != confirmPassword) {
                                Text(stringResource(R.string.unlock_passwords_mismatch))
                            }
                        },
                    )
                }
            }

            Spacer(modifier = Modifier.height(24.dp))

            if (uiState is UnlockUiState.Loading) {
                CircularProgressIndicator()
            } else {
                Button(
                    onClick = {
                        if (isNewDatabase || showConfirmField) {
                            if (password == confirmPassword && password.isNotEmpty()) {
                                viewModel.createDatabase(defaultDbPath, password)
                            }
                        } else {
                            viewModel.unlock(defaultDbPath, password)
                        }
                    },
                    modifier = Modifier.fillMaxWidth(),
                    enabled = password.isNotEmpty() && (!showConfirmField || password == confirmPassword),
                ) {
                    Text(
                        stringResource(
                            if (isNewDatabase || showConfirmField) R.string.unlock_button_create else R.string.unlock_button_unlock,
                        ),
                    )
                }

                if (canBiometric) {
                    Spacer(modifier = Modifier.height(12.dp))
                    OutlinedButton(
                        onClick = {
                            if (activity != null) {
                                viewModel.unlockWithBiometric(activity, defaultDbPath)
                            }
                        },
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Icon(Icons.Default.Fingerprint, contentDescription = stringResource(R.string.unlock_button_biometric))
                        Spacer(modifier = Modifier.size(8.dp))
                        Text(stringResource(R.string.unlock_button_biometric))
                    }
                }

                if (!isNewDatabase && !showConfirmField) {
                    Spacer(modifier = Modifier.height(8.dp))
                    OutlinedButton(
                        onClick = { showConfirmField = true },
                        modifier = Modifier.fillMaxWidth(),
                    ) {
                        Text(stringResource(R.string.unlock_button_create_new))
                    }
                }
            }
        }
    }
}
