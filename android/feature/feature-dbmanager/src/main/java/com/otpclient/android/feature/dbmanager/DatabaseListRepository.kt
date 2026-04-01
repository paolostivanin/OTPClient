package com.otpclient.android.feature.dbmanager

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import dagger.hilt.android.qualifiers.ApplicationContext
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import kotlinx.serialization.Serializable
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import javax.inject.Inject
import javax.inject.Singleton

private val Context.databaseListStore: DataStore<Preferences> by preferencesDataStore(name = "database_list")

@Serializable
data class DatabaseEntry(
    val name: String,
    val path: String,
)

@Singleton
class DatabaseListRepository @Inject constructor(
    @ApplicationContext private val context: Context,
) {
    private val key = stringPreferencesKey("databases")

    val databases: Flow<List<DatabaseEntry>> = context.databaseListStore.data.map { prefs ->
        val json = prefs[key] ?: return@map emptyList()
        try {
            Json.decodeFromString<List<DatabaseEntry>>(json)
        } catch (_: Exception) {
            emptyList()
        }
    }

    suspend fun addDatabase(entry: DatabaseEntry) {
        context.databaseListStore.edit { prefs ->
            val current = getCurrentList(prefs)
            if (current.none { it.path == entry.path }) {
                prefs[key] = Json.encodeToString(current + entry)
            }
        }
    }

    suspend fun removeDatabase(path: String) {
        context.databaseListStore.edit { prefs ->
            val current = getCurrentList(prefs)
            prefs[key] = Json.encodeToString(current.filter { it.path != path })
        }
    }

    suspend fun renameDatabase(path: String, newName: String) {
        context.databaseListStore.edit { prefs ->
            val current = getCurrentList(prefs)
            val updated = current.map {
                if (it.path == path) it.copy(name = newName) else it
            }
            prefs[key] = Json.encodeToString(updated)
        }
    }

    private fun getCurrentList(prefs: Preferences): List<DatabaseEntry> {
        val json = prefs[key] ?: return emptyList()
        return try {
            Json.decodeFromString(json)
        } catch (_: Exception) {
            emptyList()
        }
    }
}
