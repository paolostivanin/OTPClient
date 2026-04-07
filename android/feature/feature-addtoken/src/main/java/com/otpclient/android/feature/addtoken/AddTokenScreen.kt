package com.otpclient.android.feature.addtoken

import android.graphics.BitmapFactory
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.filled.Image
import androidx.compose.material.icons.filled.QrCodeScanner
import androidx.compose.material3.Button
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.ExposedDropdownMenuBox
import androidx.compose.material3.ExposedDropdownMenuDefaults
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.MenuAnchorType
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.SnackbarHost
import androidx.compose.material3.SnackbarHostState
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.google.mlkit.vision.barcode.BarcodeScanning
import com.google.mlkit.vision.common.InputImage
import com.otpclient.android.core.importexport.OtpauthUri
import kotlinx.coroutines.launch
import kotlinx.coroutines.tasks.await

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AddTokenScreen(
    onBack: () -> Unit,
    onScanQr: () -> Unit = {},
    scannedUri: String? = null,
    editIndex: Int? = null,
    initialEntry: com.otpclient.android.core.model.OtpEntry? = null,
    viewModel: AddTokenViewModel = hiltViewModel(),
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val snackbarHostState = remember { SnackbarHostState() }
    val isEditMode = editIndex != null
    val context = LocalContext.current
    val scope = rememberCoroutineScope()

    var type by rememberSaveable { mutableStateOf(initialEntry?.type ?: "TOTP") }
    var issuer by rememberSaveable { mutableStateOf(initialEntry?.issuer ?: "") }
    var label by rememberSaveable { mutableStateOf(initialEntry?.label ?: "") }
    var secret by rememberSaveable { mutableStateOf(initialEntry?.secret ?: "") }
    var algorithm by rememberSaveable { mutableStateOf(initialEntry?.algo ?: "SHA1") }
    var digits by rememberSaveable { mutableIntStateOf(initialEntry?.digits ?: 6) }
    var period by rememberSaveable { mutableIntStateOf(initialEntry?.period ?: 30) }
    var counter by rememberSaveable { mutableLongStateOf(initialEntry?.counter ?: 0L) }

    val imagePickerLauncher = rememberLauncherForActivityResult(
        contract = ActivityResultContracts.GetContent(),
    ) { uri ->
        uri ?: return@rememberLauncherForActivityResult
        scope.launch {
            try {
                val inputStream = context.contentResolver.openInputStream(uri)
                val bitmap = BitmapFactory.decodeStream(inputStream)
                inputStream?.close()
                if (bitmap == null) {
                    snackbarHostState.showSnackbar("Failed to load image")
                    return@launch
                }
                val image = InputImage.fromBitmap(bitmap, 0)
                val scanner = BarcodeScanning.getClient()
                val barcodes = scanner.process(image).await()
                val otpauthUrl = barcodes.firstOrNull { it.rawValue?.startsWith("otpauth://") == true }?.rawValue
                if (otpauthUrl != null) {
                    OtpauthUri.parse(otpauthUrl)?.let { entry ->
                        type = entry.type
                        issuer = entry.issuer
                        label = entry.label
                        secret = entry.secret
                        algorithm = entry.algo
                        digits = entry.digits
                        period = entry.period
                        counter = entry.counter
                    } ?: snackbarHostState.showSnackbar("Invalid otpauth URI")
                } else {
                    snackbarHostState.showSnackbar("No QR code found in image")
                }
                scanner.close()
            } catch (e: Exception) {
                snackbarHostState.showSnackbar("Error scanning image: ${e.message}")
            }
        }
    }

    LaunchedEffect(scannedUri) {
        scannedUri?.let { uri ->
            OtpauthUri.parse(uri)?.let { entry ->
                type = entry.type
                issuer = entry.issuer
                label = entry.label
                secret = entry.secret
                algorithm = entry.algo
                digits = entry.digits
                period = entry.period
                counter = entry.counter
            }
        }
    }

    LaunchedEffect(uiState) {
        when (val state = uiState) {
            is AddTokenUiState.Success -> onBack()
            is AddTokenUiState.Error -> snackbarHostState.showSnackbar(state.message)
            else -> {}
        }
    }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(if (isEditMode) R.string.addtoken_edit_title else R.string.addtoken_title)) },
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
            Button(
                onClick = onScanQr,
                modifier = Modifier.fillMaxWidth(),
            ) {
                Icon(Icons.Default.QrCodeScanner, contentDescription = stringResource(R.string.addtoken_scan_qr))
                Spacer(modifier = Modifier.width(8.dp))
                Text(stringResource(R.string.addtoken_scan_qr))
            }

            Spacer(modifier = Modifier.height(8.dp))

            OutlinedButton(
                onClick = { imagePickerLauncher.launch("image/*") },
                modifier = Modifier.fillMaxWidth(),
            ) {
                Icon(Icons.Default.Image, contentDescription = stringResource(R.string.addtoken_import_from_image))
                Spacer(modifier = Modifier.width(8.dp))
                Text(stringResource(R.string.addtoken_import_from_image))
            }

            Spacer(modifier = Modifier.height(16.dp))

            SingleChoiceSegmentedButtonRow(modifier = Modifier.fillMaxWidth()) {
                listOf("TOTP", "HOTP").forEachIndexed { index, option ->
                    SegmentedButton(
                        selected = type == option,
                        onClick = { type = option },
                        shape = SegmentedButtonDefaults.itemShape(index = index, count = 2),
                    ) {
                        Text(option)
                    }
                }
            }

            Spacer(modifier = Modifier.height(16.dp))

            OutlinedTextField(
                value = issuer,
                onValueChange = { issuer = it },
                label = { Text(stringResource(R.string.addtoken_issuer)) },
                placeholder = { Text(stringResource(R.string.addtoken_issuer_hint)) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )

            Spacer(modifier = Modifier.height(12.dp))

            OutlinedTextField(
                value = label,
                onValueChange = { label = it },
                label = { Text(stringResource(R.string.addtoken_label)) },
                placeholder = { Text(stringResource(R.string.addtoken_label_hint)) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )

            Spacer(modifier = Modifier.height(12.dp))

            OutlinedTextField(
                value = secret,
                onValueChange = { secret = it },
                label = { Text(stringResource(R.string.addtoken_secret)) },
                placeholder = { Text(stringResource(R.string.addtoken_secret_hint)) },
                singleLine = true,
                modifier = Modifier.fillMaxWidth(),
            )

            Spacer(modifier = Modifier.height(16.dp))

            var algoExpanded by remember { mutableStateOf(false) }
            ExposedDropdownMenuBox(
                expanded = algoExpanded,
                onExpandedChange = { algoExpanded = it },
            ) {
                OutlinedTextField(
                    value = algorithm,
                    onValueChange = {},
                    readOnly = true,
                    label = { Text(stringResource(R.string.addtoken_algorithm)) },
                    trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = algoExpanded) },
                    modifier = Modifier
                        .fillMaxWidth()
                        .menuAnchor(MenuAnchorType.PrimaryNotEditable),
                )
                ExposedDropdownMenu(
                    expanded = algoExpanded,
                    onDismissRequest = { algoExpanded = false },
                ) {
                    listOf("SHA1", "SHA256", "SHA512").forEach { algo ->
                        DropdownMenuItem(
                            text = { Text(algo) },
                            onClick = {
                                algorithm = algo
                                algoExpanded = false
                            },
                        )
                    }
                }
            }

            Spacer(modifier = Modifier.height(12.dp))

            Row(modifier = Modifier.fillMaxWidth()) {
                var digitsExpanded by remember { mutableStateOf(false) }
                ExposedDropdownMenuBox(
                    expanded = digitsExpanded,
                    onExpandedChange = { digitsExpanded = it },
                    modifier = Modifier.weight(1f),
                ) {
                    OutlinedTextField(
                        value = digits.toString(),
                        onValueChange = {},
                        readOnly = true,
                        label = { Text(stringResource(R.string.addtoken_digits)) },
                        trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = digitsExpanded) },
                        modifier = Modifier.menuAnchor(MenuAnchorType.PrimaryNotEditable),
                    )
                    ExposedDropdownMenu(
                        expanded = digitsExpanded,
                        onDismissRequest = { digitsExpanded = false },
                    ) {
                        listOf(6, 7, 8).forEach { d ->
                            DropdownMenuItem(
                                text = { Text(d.toString()) },
                                onClick = {
                                    digits = d
                                    digitsExpanded = false
                                },
                            )
                        }
                    }
                }

                Spacer(modifier = Modifier.width(12.dp))

                if (type == "TOTP") {
                    OutlinedTextField(
                        value = period.toString(),
                        onValueChange = { period = it.toIntOrNull() ?: 30 },
                        label = { Text(stringResource(R.string.addtoken_period)) },
                        singleLine = true,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                        modifier = Modifier.weight(1f),
                    )
                } else {
                    OutlinedTextField(
                        value = counter.toString(),
                        onValueChange = { counter = it.toLongOrNull() ?: 0L },
                        label = { Text(stringResource(R.string.addtoken_counter)) },
                        singleLine = true,
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number),
                        modifier = Modifier.weight(1f),
                    )
                }
            }

            Spacer(modifier = Modifier.height(24.dp))

            Button(
                onClick = {
                    viewModel.saveToken(
                        type = type,
                        issuer = issuer,
                        label = label,
                        secret = secret,
                        algorithm = algorithm,
                        digits = digits,
                        period = period,
                        counter = counter,
                        editIndex = editIndex,
                    )
                },
                modifier = Modifier.fillMaxWidth(),
                enabled = uiState !is AddTokenUiState.Saving,
            ) {
                Text(stringResource(if (isEditMode) R.string.addtoken_save_button else R.string.addtoken_button))
            }

            Spacer(modifier = Modifier.height(16.dp))
        }
    }
}
