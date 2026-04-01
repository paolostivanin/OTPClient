package com.otpclient.android.lifecycle

import androidx.lifecycle.DefaultLifecycleObserver
import androidx.lifecycle.LifecycleOwner
import androidx.lifecycle.ProcessLifecycleOwner
import com.otpclient.android.core.database.DatabaseRepository
import com.otpclient.android.feature.settings.SettingsRepository
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import javax.inject.Inject
import javax.inject.Singleton

@Singleton
class AppLockManager @Inject constructor(
    private val databaseRepository: DatabaseRepository,
    private val settingsRepository: SettingsRepository,
    private val scope: CoroutineScope,
) : DefaultLifecycleObserver {

    private var lockJob: Job? = null

    private val _lockEvent = MutableStateFlow(0L)
    val lockEvent: StateFlow<Long> = _lockEvent.asStateFlow()

    fun register() {
        ProcessLifecycleOwner.get().lifecycle.addObserver(this)
    }

    override fun onStop(owner: LifecycleOwner) {
        // App moved to background
        lockJob?.cancel()
        lockJob = scope.launch {
            val settings = settingsRepository.settings.first()
            val seconds = settings.autoLockSeconds
            if (seconds > 0 && databaseRepository.isUnlocked) {
                delay(seconds * 1000L)
                databaseRepository.lock()
                _lockEvent.value = System.currentTimeMillis()
            }
        }
    }

    override fun onStart(owner: LifecycleOwner) {
        // App came to foreground — cancel pending lock if user returned in time
        lockJob?.cancel()
        lockJob = null
    }
}
