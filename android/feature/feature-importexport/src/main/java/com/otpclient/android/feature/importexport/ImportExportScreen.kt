package com.otpclient.android.feature.importexport

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
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
import androidx.compose.material.icons.filled.Download
import androidx.compose.material.icons.filled.Upload
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
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
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun ImportExportScreen(
    onBack: () -> Unit,
    viewModel: ImportExportViewModel = hiltViewModel(),
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val snackbarHostState = remember { SnackbarHostState() }
    val context = LocalContext.current

    var selectedFormat by rememberSaveable { mutableStateOf<ImportExportFormat?>(null) }
    var isImport by rememberSaveable { mutableStateOf(true) }
    var showPasswordDialog by rememberSaveable { mutableStateOf(false) }
    var pendingFilePath by rememberSaveable { mutableStateOf<String?>(null) }

    val importFilePicker = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.OpenDocument(),
    ) { uri: Uri? ->
        if (uri != null && selectedFormat != null) {
            val inputStream = context.contentResolver.openInputStream(uri)
            val cacheFile = java.io.File(context.cacheDir, "import_temp")
            inputStream?.use { input ->
                cacheFile.outputStream().use { output -> input.copyTo(output) }
            }

            val needsPassword = selectedFormat in listOf(
                ImportExportFormat.AEGIS_ENCRYPTED,
                ImportExportFormat.AUTHPRO_ENCRYPTED,
                ImportExportFormat.TWOFAS_ENCRYPTED,
            )

            if (needsPassword) {
                pendingFilePath = cacheFile.absolutePath
                showPasswordDialog = true
            } else {
                viewModel.importFrom(cacheFile.absolutePath, selectedFormat!!, null)
            }
        }
    }

    val exportFilePicker = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.CreateDocument("*/*"),
    ) { uri: Uri? ->
        if (uri != null && selectedFormat != null) {
            val cacheFile = java.io.File(context.cacheDir, "export_temp")

            val needsPassword = selectedFormat in listOf(
                ImportExportFormat.AEGIS_ENCRYPTED,
                ImportExportFormat.AUTHPRO_ENCRYPTED,
                ImportExportFormat.TWOFAS_ENCRYPTED,
            )

            if (needsPassword) {
                pendingFilePath = cacheFile.absolutePath
                showPasswordDialog = true
            } else {
                viewModel.exportTo(cacheFile.absolutePath, selectedFormat!!, null)
                cacheFile.inputStream().use { input ->
                    context.contentResolver.openOutputStream(uri)?.use { output ->
                        input.copyTo(output)
                    }
                }
            }
        }
    }

    LaunchedEffect(uiState) {
        when (val state = uiState) {
            is ImportExportUiState.Success -> {
                snackbarHostState.showSnackbar(state.message)
                viewModel.resetState()
            }
            is ImportExportUiState.Error -> {
                snackbarHostState.showSnackbar(state.message)
                viewModel.resetState()
            }
            else -> {}
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.importexport_title)) },
                navigationIcon = {
                    IconButton(onClick = onBack) {
                        Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = stringResource(R.string.content_desc_back))
                    }
                },
            )
        },
        snackbarHost = { SnackbarHost(snackbarHostState) },
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(horizontal = 16.dp)
                .verticalScroll(rememberScrollState()),
        ) {
            Text(
                text = stringResource(R.string.importexport_section_import),
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(vertical = 8.dp),
            )

            importFormats.forEach { format ->
                FormatCard(
                    format = format,
                    icon = { Icon(Icons.Default.Download, contentDescription = null) },
                    enabled = uiState !is ImportExportUiState.Processing,
                    onClick = {
                        selectedFormat = format
                        isImport = true
                        importFilePicker.launch(arrayOf("*/*"))
                    },
                )
                Spacer(modifier = Modifier.height(4.dp))
            }

            Spacer(modifier = Modifier.height(16.dp))

            Text(
                text = stringResource(R.string.importexport_section_export),
                style = MaterialTheme.typography.titleMedium,
                modifier = Modifier.padding(vertical = 8.dp),
            )

            exportFormats.forEach { format ->
                FormatCard(
                    format = format,
                    icon = { Icon(Icons.Default.Upload, contentDescription = null) },
                    enabled = uiState !is ImportExportUiState.Processing,
                    onClick = {
                        selectedFormat = format
                        isImport = false
                        exportFilePicker.launch("otpclient_export")
                    },
                )
                Spacer(modifier = Modifier.height(4.dp))
            }

            if (uiState is ImportExportUiState.Processing) {
                Spacer(modifier = Modifier.height(16.dp))
                CircularProgressIndicator(modifier = Modifier.align(Alignment.CenterHorizontally))
            }

            Spacer(modifier = Modifier.height(16.dp))
        }
    }

    if (showPasswordDialog) {
        PasswordDialog(
            onConfirm = { password ->
                showPasswordDialog = false
                val path = pendingFilePath ?: return@PasswordDialog
                if (isImport) {
                    viewModel.importFrom(path, selectedFormat!!, password)
                } else {
                    viewModel.exportTo(path, selectedFormat!!, password)
                }
            },
            onDismiss = {
                showPasswordDialog = false
                pendingFilePath = null
            },
        )
    }
}

@Composable
private fun FormatCard(
    format: ImportExportFormat,
    icon: @Composable () -> Unit,
    enabled: Boolean,
    onClick: () -> Unit,
) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .semantics { contentDescription = format.displayName }
            .clickable(enabled = enabled, onClick = onClick),
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
            icon()
            Spacer(modifier = Modifier.width(12.dp))
            Text(text = format.displayName, style = MaterialTheme.typography.bodyLarge)
        }
    }
}

@Composable
private fun PasswordDialog(
    onConfirm: (String) -> Unit,
    onDismiss: () -> Unit,
) {
    var password by rememberSaveable { mutableStateOf("") }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text(stringResource(R.string.importexport_password_title)) },
        text = {
            OutlinedTextField(
                value = password,
                onValueChange = { password = it },
                label = { Text(stringResource(R.string.importexport_password_label)) },
                singleLine = true,
                visualTransformation = PasswordVisualTransformation(),
                modifier = Modifier.fillMaxWidth(),
            )
        },
        confirmButton = {
            TextButton(
                onClick = { onConfirm(password) },
                enabled = password.isNotEmpty(),
            ) { Text(stringResource(R.string.importexport_ok)) }
        },
        dismissButton = {
            TextButton(onClick = onDismiss) { Text(stringResource(R.string.importexport_cancel)) }
        },
    )
}
