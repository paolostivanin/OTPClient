package com.otpclient.android.feature.tokenlist

import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Bitmap
import android.widget.Toast
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.Image
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Add
import androidx.compose.material.icons.filled.ContentCopy
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Edit
import androidx.compose.material.icons.filled.QrCode2
import androidx.compose.material.icons.filled.ImportExport
import androidx.compose.material.icons.filled.Lock
import androidx.compose.material.icons.filled.Menu
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FloatingActionButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SearchBar
import androidx.compose.material3.SearchBarDefaults
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.hilt.navigation.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import com.google.zxing.BarcodeFormat
import com.google.zxing.qrcode.QRCodeWriter
import com.otpclient.android.core.importexport.OtpauthUri

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun TokenListScreen(
    onAddToken: () -> Unit,
    onEditToken: (Int) -> Unit = {},
    onSettings: () -> Unit,
    onImportExport: () -> Unit = {},
    onDatabaseManager: () -> Unit = {},
    onLocked: () -> Unit,
    viewModel: TokenListViewModel = hiltViewModel(),
) {
    val uiState by viewModel.uiState.collectAsStateWithLifecycle()
    val searchQuery by viewModel.searchQuery.collectAsStateWithLifecycle()
    val pendingConflict by viewModel.pendingConflict.collectAsStateWithLifecycle()
    val context = LocalContext.current
    var searchActive by rememberSaveable { mutableStateOf(false) }
    var deleteTarget by remember { mutableStateOf<TokenUiModel?>(null) }
    var qrTarget by remember { mutableStateOf<TokenUiModel?>(null) }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.tokenlist_title)) },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surface,
                ),
                navigationIcon = {
                    IconButton(onClick = onDatabaseManager) {
                        Icon(Icons.Default.Menu, contentDescription = stringResource(R.string.tokenlist_databases))
                    }
                },
                actions = {
                    IconButton(onClick = { searchActive = !searchActive }) {
                        Icon(Icons.Default.Search, contentDescription = stringResource(R.string.tokenlist_search))
                    }
                    IconButton(onClick = onImportExport) {
                        Icon(Icons.Default.ImportExport, contentDescription = stringResource(R.string.tokenlist_import_export))
                    }
                    IconButton(onClick = onSettings) {
                        Icon(Icons.Default.Settings, contentDescription = stringResource(R.string.tokenlist_settings))
                    }
                    IconButton(onClick = {
                        viewModel.lock()
                        onLocked()
                    }) {
                        Icon(Icons.Default.Lock, contentDescription = stringResource(R.string.tokenlist_lock))
                    }
                },
            )
        },
        floatingActionButton = {
            FloatingActionButton(onClick = onAddToken) {
                Icon(Icons.Default.Add, contentDescription = stringResource(R.string.tokenlist_add_token))
            }
        },
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding),
        ) {
            if (searchActive) {
                SearchBar(
                    inputField = {
                        SearchBarDefaults.InputField(
                            query = searchQuery,
                            onQueryChange = viewModel::onSearchQueryChanged,
                            onSearch = { searchActive = false },
                            expanded = false,
                            onExpandedChange = {},
                            placeholder = { Text(stringResource(R.string.tokenlist_search_placeholder)) },
                        )
                    },
                    expanded = false,
                    onExpandedChange = {},
                    modifier = Modifier
                        .fillMaxWidth()
                        .padding(horizontal = 16.dp),
                ) {}
            }

            when (val state = uiState) {
                is TokenListUiState.Loading -> {
                    Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                        CircularProgressIndicator()
                    }
                }
                is TokenListUiState.Loaded -> {
                    if (state.tokens.isEmpty()) {
                        Box(modifier = Modifier.fillMaxSize(), contentAlignment = Alignment.Center) {
                            Column(horizontalAlignment = Alignment.CenterHorizontally) {
                                Text(
                                    text = stringResource(R.string.tokenlist_no_tokens_title),
                                    style = MaterialTheme.typography.titleLarge,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                                Spacer(modifier = Modifier.height(8.dp))
                                Text(
                                    text = stringResource(R.string.tokenlist_no_tokens_subtitle),
                                    style = MaterialTheme.typography.bodyMedium,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                        }
                    } else {
                        LazyColumn(
                            modifier = Modifier.fillMaxSize(),
                            verticalArrangement = Arrangement.spacedBy(8.dp),
                            contentPadding = androidx.compose.foundation.layout.PaddingValues(
                                start = 16.dp, end = 16.dp, top = 8.dp, bottom = 88.dp,
                            ),
                        ) {
                            items(state.tokens, key = { it.index }) { token ->
                                TokenCard(
                                    token = token,
                                    onCopy = {
                                        copyToClipboard(context, token.currentOtp, token.entry.issuer)
                                    },
                                    onEdit = { onEditToken(token.index) },
                                    onDelete = { deleteTarget = token },
                                    onShowQr = { qrTarget = token },
                                    onRefreshHotp = {
                                        if (token.entry.type == "HOTP") {
                                            viewModel.incrementHotpCounter(token.index)
                                        }
                                    },
                                )
                            }
                        }
                    }
                }
            }
        }
    }

    deleteTarget?.let { token ->
        AlertDialog(
            onDismissRequest = { deleteTarget = null },
            title = { Text(stringResource(R.string.tokenlist_delete_title)) },
            text = {
                Text(
                    stringResource(
                        R.string.tokenlist_delete_message,
                        token.entry.issuer.ifEmpty { token.entry.label },
                    ),
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    viewModel.deleteEntry(token.index)
                    deleteTarget = null
                }) { Text(stringResource(R.string.tokenlist_delete)) }
            },
            dismissButton = {
                TextButton(onClick = { deleteTarget = null }) { Text(stringResource(R.string.tokenlist_cancel)) }
            },
        )
    }

    qrTarget?.let { token ->
        val qrBitmap = remember(token) {
            generateQrBitmap(OtpauthUri.toUri(token.entry), 512)
        }
        AlertDialog(
            onDismissRequest = { qrTarget = null },
            title = { Text(stringResource(R.string.tokenlist_qr_dialog_title)) },
            text = {
                Column(horizontalAlignment = Alignment.CenterHorizontally, modifier = Modifier.fillMaxWidth()) {
                    Text(
                        text = token.entry.issuer.ifEmpty { token.entry.label },
                        style = MaterialTheme.typography.titleMedium,
                    )
                    Spacer(modifier = Modifier.height(16.dp))
                    if (qrBitmap != null) {
                        Image(
                            bitmap = qrBitmap.asImageBitmap(),
                            contentDescription = stringResource(R.string.tokenlist_show_qr),
                            modifier = Modifier.size(256.dp),
                        )
                    }
                }
            },
            confirmButton = {
                TextButton(onClick = { qrTarget = null }) { Text(stringResource(R.string.tokenlist_close)) }
            },
        )
    }

    pendingConflict?.let {
        AlertDialog(
            onDismissRequest = {},
            title = { Text(stringResource(R.string.tokenlist_sync_conflict_title)) },
            text = { Text(stringResource(R.string.tokenlist_sync_conflict_message)) },
            confirmButton = {
                TextButton(onClick = { viewModel.resolveConflict(keepLocal = true) }) {
                    Text(stringResource(R.string.tokenlist_sync_keep_local))
                }
            },
            dismissButton = {
                TextButton(onClick = { viewModel.resolveConflict(keepLocal = false) }) {
                    Text(stringResource(R.string.tokenlist_sync_keep_remote))
                }
            },
        )
    }
}

@Composable
private fun TokenCard(
    token: TokenUiModel,
    onCopy: () -> Unit,
    onEdit: () -> Unit,
    onDelete: () -> Unit,
    onShowQr: () -> Unit,
    onRefreshHotp: () -> Unit,
) {
    val progress = token.remainingSeconds?.let {
        it.toFloat() / token.period.toFloat()
    }
    val animatedProgress by animateFloatAsState(
        targetValue = progress ?: 1f,
        label = "countdown",
    )

    val label = token.entry.issuer.ifEmpty { token.entry.label }
    val remainingDesc = token.remainingSeconds?.let {
        stringResource(R.string.tokenlist_remaining_seconds, it)
    } ?: ""
    val cardDesc = stringResource(R.string.tokenlist_card_description, label, token.currentOtp, remainingDesc)

    Card(
        modifier = Modifier
            .fillMaxWidth()
            .semantics { contentDescription = cardDesc }
            .clickable { onCopy() },
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
            Column(modifier = Modifier.weight(1f)) {
                Text(
                    text = token.entry.issuer.ifEmpty { token.entry.label },
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
                if (token.entry.issuer.isNotEmpty() && token.entry.label.isNotEmpty()) {
                    Text(
                        text = token.entry.label,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                    )
                }
                Spacer(modifier = Modifier.height(4.dp))
                Text(
                    text = formatOtp(token.currentOtp),
                    style = MaterialTheme.typography.headlineMedium.copy(
                        fontFamily = FontFamily.Monospace,
                        letterSpacing = 4.sp,
                    ),
                    color = MaterialTheme.colorScheme.primary,
                    fontWeight = FontWeight.Bold,
                )
                if (token.nextOtp != null) {
                    Text(
                        text = "Next: ${formatOtp(token.nextOtp)}",
                        style = MaterialTheme.typography.bodySmall.copy(
                            fontFamily = FontFamily.Monospace,
                        ),
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
            }

            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                if (token.remainingSeconds != null) {
                    Box(contentAlignment = Alignment.Center) {
                        CircularProgressIndicator(
                            progress = { animatedProgress },
                            modifier = Modifier.size(40.dp),
                            strokeWidth = 3.dp,
                            color = when {
                                token.remainingSeconds <= token.period / 6 -> MaterialTheme.colorScheme.error
                                token.remainingSeconds <= token.period / 2 -> Color(0xFFFF9800)
                                else -> Color(0xFF4CAF50)
                            },
                            trackColor = MaterialTheme.colorScheme.surfaceVariant,
                        )
                        Text(
                            text = "${token.remainingSeconds}",
                            style = MaterialTheme.typography.labelMedium,
                            fontWeight = FontWeight.Bold,
                        )
                    }
                } else {
                    IconButton(onClick = onRefreshHotp) {
                        Icon(Icons.Default.Refresh, contentDescription = stringResource(R.string.tokenlist_next_code))
                    }
                }

                Spacer(modifier = Modifier.height(4.dp))

                Row {
                    IconButton(onClick = onCopy, modifier = Modifier.size(36.dp)) {
                        Icon(
                            Icons.Default.ContentCopy,
                            contentDescription = stringResource(R.string.tokenlist_copy),
                            modifier = Modifier.size(16.dp),
                        )
                    }
                    IconButton(onClick = onShowQr, modifier = Modifier.size(36.dp)) {
                        Icon(
                            Icons.Default.QrCode2,
                            contentDescription = stringResource(R.string.tokenlist_show_qr),
                            modifier = Modifier.size(16.dp),
                        )
                    }
                    IconButton(onClick = onEdit, modifier = Modifier.size(36.dp)) {
                        Icon(
                            Icons.Default.Edit,
                            contentDescription = stringResource(R.string.tokenlist_edit),
                            modifier = Modifier.size(16.dp),
                        )
                    }
                    IconButton(onClick = onDelete, modifier = Modifier.size(36.dp)) {
                        Icon(
                            Icons.Default.Delete,
                            contentDescription = stringResource(R.string.tokenlist_delete),
                            modifier = Modifier.size(16.dp),
                            tint = MaterialTheme.colorScheme.error,
                        )
                    }
                }
            }
        }
    }
}

private fun formatOtp(otp: String): String {
    if (otp.length <= 4) return otp
    val mid = otp.length / 2
    return "${otp.substring(0, mid)} ${otp.substring(mid)}"
}

private fun generateQrBitmap(content: String, size: Int): Bitmap? {
    return try {
        val bitMatrix = QRCodeWriter().encode(content, BarcodeFormat.QR_CODE, size, size)
        val bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        for (x in 0 until size) {
            for (y in 0 until size) {
                bitmap.setPixel(x, y, if (bitMatrix[x, y]) android.graphics.Color.BLACK else android.graphics.Color.WHITE)
            }
        }
        bitmap
    } catch (_: Exception) {
        null
    }
}

private fun copyToClipboard(context: Context, otp: String, label: String) {
    val clipboard = context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
    val clip = ClipData.newPlainText("OTP", otp)
    clipboard.setPrimaryClip(clip)
    Toast.makeText(
        context,
        context.getString(R.string.tokenlist_copied, label),
        Toast.LENGTH_SHORT,
    ).show()
}
