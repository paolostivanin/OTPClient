package com.otpclient.android

import android.app.Application
import com.otpclient.android.lifecycle.AppLockManager
import dagger.hilt.android.HiltAndroidApp
import javax.inject.Inject

@HiltAndroidApp
class OTPClientApplication : Application() {

    @Inject
    lateinit var appLockManager: AppLockManager

    override fun onCreate() {
        super.onCreate()
        appLockManager.register()
    }
}
