package com.otpclient.android.ui

import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.rememberNavController
import androidx.navigation.NavType
import androidx.navigation.navArgument
import com.otpclient.android.core.database.DatabaseRepository
import com.otpclient.android.core.importexport.GoogleMigrationProvider
import com.otpclient.android.core.importexport.OtpauthUri
import com.otpclient.android.feature.addtoken.AddTokenScreen
import com.otpclient.android.feature.addtoken.qr.QrScannerScreen
import com.otpclient.android.feature.dbmanager.DatabaseManagerScreen
import com.otpclient.android.feature.importexport.ImportExportScreen
import com.otpclient.android.feature.settings.SettingsScreen
import com.otpclient.android.feature.tokenlist.TokenListScreen
import com.otpclient.android.feature.unlock.UnlockScreen
import com.otpclient.android.lifecycle.AppLockManager

@Composable
fun OTPClientApp(
    defaultDbPath: String,
    appLockManager: AppLockManager? = null,
    databaseRepository: DatabaseRepository? = null,
) {
    val navController = rememberNavController()

    // Observe lock events from auto-lock
    if (appLockManager != null) {
        val lockEvent by appLockManager.lockEvent.collectAsStateWithLifecycle()
        LaunchedEffect(lockEvent) {
            if (lockEvent > 0) {
                navController.navigate("unlock") {
                    popUpTo(0) { inclusive = true }
                }
            }
        }
    }

    NavHost(
        navController = navController,
        startDestination = "unlock",
    ) {
        composable("unlock") {
            UnlockScreen(
                defaultDbPath = defaultDbPath,
                onUnlocked = {
                    navController.navigate("tokenlist") {
                        popUpTo("unlock") { inclusive = true }
                    }
                },
            )
        }

        composable("tokenlist") {
            TokenListScreen(
                onAddToken = { navController.navigate("addtoken") },
                onEditToken = { index -> navController.navigate("edittoken/$index") },
                onSettings = { navController.navigate("settings") },
                onImportExport = { navController.navigate("importexport") },
                onDatabaseManager = { navController.navigate("dbmanager") },
                onLocked = {
                    navController.navigate("unlock") {
                        popUpTo("tokenlist") { inclusive = true }
                    }
                },
            )
        }

        composable("addtoken") { backStackEntry ->
            val scannedUri = backStackEntry.savedStateHandle.get<String>("scanned_uri")
            backStackEntry.savedStateHandle.remove<String>("scanned_uri")
            AddTokenScreen(
                onBack = { navController.popBackStack() },
                onScanQr = { navController.navigate("qrscanner") },
                scannedUri = scannedUri,
            )
        }

        composable(
            "edittoken/{index}",
            arguments = listOf(navArgument("index") { type = NavType.IntType }),
        ) { backStackEntry ->
            val index = backStackEntry.arguments?.getInt("index") ?: 0
            val entry = databaseRepository?.entries?.value?.getOrNull(index)
            AddTokenScreen(
                onBack = { navController.popBackStack() },
                editIndex = index,
                initialEntry = entry,
            )
        }

        composable("qrscanner") {
            QrScannerScreen(
                onQrCodeScanned = { value ->
                    // Handle both otpauth:// and otpauth-migration:// URIs
                    if (value.startsWith("otpauth-migration://")) {
                        // Google migration returns multiple entries — go back and let import handle it
                        // For now, add first entry
                        val entries = GoogleMigrationProvider.parseFromUri(value)
                        if (entries.isNotEmpty()) {
                            navController.previousBackStackEntry
                                ?.savedStateHandle
                                ?.set("scanned_uri", OtpauthUri.toUri(entries.first()))
                        }
                    } else {
                        navController.previousBackStackEntry
                            ?.savedStateHandle
                            ?.set("scanned_uri", value)
                    }
                    navController.popBackStack()
                },
                onBack = { navController.popBackStack() },
            )
        }

        composable("dbmanager") {
            DatabaseManagerScreen(
                onBack = { navController.popBackStack() },
                onDatabaseSelected = { path ->
                    // Navigate to unlock with the selected database
                    navController.navigate("unlock") {
                        popUpTo(0) { inclusive = true }
                    }
                },
            )
        }

        composable("settings") {
            SettingsScreen(
                onBack = { navController.popBackStack() },
            )
        }

        composable("importexport") {
            ImportExportScreen(
                onBack = { navController.popBackStack() },
            )
        }
    }
}
