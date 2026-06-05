// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// Thin NativeActivity wrapper. Two jobs:
//   1. Surface the runtime CAMERA permission dialog at first launch (CNSDK's
//      face tracker needs the front camera).
//   2. Push the authoritative 4-way display rotation to the native code on
//      launch and on every rotation (including 180° flips, via a
//      DisplayListener). The renderer can't derive the true rotation from its
//      own surface, and Configuration.orientation only distinguishes
//      portrait/landscape (not ROTATION_0 vs ROTATION_180), so we feed it
//      Surface.rotation here. Orientation is NOT locked — the runtime + CNSDK
//      adapt per orientation, the way the Leia viewer does.

package com.displayxr.cube_handle_vk_android

import android.Manifest
import android.app.NativeActivity
import android.content.Context
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.hardware.display.DisplayManager
import android.os.Bundle

class MainActivity : NativeActivity() {

    companion object {
        private const val REQUEST_CAMERA = 1

        // Load the native lib into the JVM so the external JNI function below
        // resolves (NativeActivity also dlopens it for android_main; this load
        // is what binds the Java_… symbol).
        init {
            System.loadLibrary("cube_handle_vk_android")
        }
    }

    // Implemented in main.cpp. rotation = Surface.ROTATION_0/90/180/270 → 0/1/2/3.
    private external fun nativeSetRotation(rotation: Int)

    private val displayListener = object : DisplayManager.DisplayListener {
        override fun onDisplayChanged(displayId: Int) = pushRotation()
        override fun onDisplayAdded(displayId: Int) {}
        override fun onDisplayRemoved(displayId: Int) {}
    }

    private fun pushRotation() {
        @Suppress("DEPRECATION")
        val rotation = windowManager.defaultDisplay.rotation  // Surface.ROTATION_*
        try {
            nativeSetRotation(rotation)
        } catch (_: Throwable) {
            // Native lib not bound yet — a later display/config change retries.
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        // Do NOT lock orientation — let all four orientations through.
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR
        super.onCreate(savedInstanceState)
        pushRotation()
        (getSystemService(Context.DISPLAY_SERVICE) as DisplayManager)
            .registerDisplayListener(displayListener, null)
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.CAMERA), REQUEST_CAMERA)
        }
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        pushRotation()
    }

    override fun onResume() {
        super.onResume()
        pushRotation()
    }

    override fun onDestroy() {
        try {
            (getSystemService(Context.DISPLAY_SERVICE) as DisplayManager)
                .unregisterDisplayListener(displayListener)
        } catch (_: Throwable) {
        }
        super.onDestroy()
    }
}
