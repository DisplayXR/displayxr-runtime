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

package com.displayxr.model_viewer_vk_android

import android.Manifest
import android.app.AlertDialog
import android.app.NativeActivity
import android.content.Context
import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.hardware.display.DisplayManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.GestureDetector
import android.view.MotionEvent
import android.widget.Toast

class MainActivity : NativeActivity() {

    companion object {
        private const val REQUEST_CAMERA = 1

        // Load the native lib into the JVM so the external JNI function below
        // resolves (NativeActivity also dlopens it for android_main; this load
        // is what binds the Java_… symbol).
        init {
            System.loadLibrary("model_viewer_vk_android")
        }
    }

    // Implemented in main.cpp. rotation = Surface.ROTATION_0/90/180/270 → 0/1/2/3.
    private external fun nativeSetRotation(rotation: Int)

    // True once xrCreateInstance failed with RUNTIME_UNAVAILABLE.
    private external fun nativeRuntimeUnavailable(): Boolean

    // True once the OpenXR instance is up (runtime reached).
    private external fun nativeXrReady(): Boolean

    // Pinch-to-zoom bridge. The runtime's MonadoView overlay covers our window
    // and is the only one that receives touch; it forwards each event here via
    // Activity.dispatchTouchEvent (Monado #499). A NativeActivity's native input
    // queue is NOT fed by dispatchTouchEvent, so we forward the coordinates to
    // native ourselves instead of relying on android_app's onInputEvent.
    private external fun nativeOnTouch(
        action: Int, count: Int, x0: Float, y0: Float, x1: Float, y1: Float,
    )

    // Double-tap cycles to the next bundled model.
    private external fun nativeCycleModel()

    private val gestureDetector by lazy {
        GestureDetector(
            this,
            object : GestureDetector.SimpleOnGestureListener() {
                override fun onDoubleTap(e: MotionEvent): Boolean {
                    try {
                        nativeCycleModel()
                    } catch (_: Throwable) {
                    }
                    return true
                }
            },
        )
    }

    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        gestureDetector.onTouchEvent(event) // double-tap → cycle model
        val n = event.pointerCount
        val x1 = if (n > 1) event.getX(1) else 0f
        val y1 = if (n > 1) event.getY(1) else 0f
        try {
            // Single-finger drag = orbit; two-finger pinch = zoom (handled native-side).
            nativeOnTouch(event.actionMasked, n, event.getX(0), event.getY(0), x1, y1)
        } catch (_: Throwable) {
            // Native lib not bound yet — ignore until it is.
        }
        return super.dispatchTouchEvent(event)
    }

    private val runtimePackage = "org.freedesktop.monado.openxr_runtime.in_process"

    // Watch the native bring-up just until it resolves: if the runtime can't be
    // reached, prompt the user to launch DisplayXR first; if it comes up, stop
    // (so the dialog can't re-fire once it's working). Bounded, not an endless
    // poll.
    private fun watchForRuntimeUnavailable() {
        val handler = Handler(Looper.getMainLooper())
        handler.postDelayed(
            object : Runnable {
                var tries = 0
                override fun run() {
                    if (isFinishing) return
                    val unavailable = try { nativeRuntimeUnavailable() } catch (_: Throwable) { false }
                    if (unavailable) {
                        showRuntimeMissingDialog()
                        return // resolved (failed)
                    }
                    val ready = try { nativeXrReady() } catch (_: Throwable) { false }
                    if (ready) return // resolved (working) — stop watching
                    if (tries++ < 15) handler.postDelayed(this, 1000)
                }
            },
            2000, // let the native side finish its retry first
        )
    }

    private fun showRuntimeMissingDialog() {
        try {
            AlertDialog.Builder(this)
                .setTitle("DisplayXR not running")
                .setMessage(
                    "Couldn't reach the DisplayXR runtime.\n\n" +
                        "Open the DisplayXR app once (it shows the logo), then reopen this app.",
                )
                .setCancelable(false)
                .setPositiveButton("Open DisplayXR") { _, _ ->
                    val intent = packageManager.getLaunchIntentForPackage(runtimePackage)
                    if (intent != null) startActivity(intent)
                    finish()
                }
                .setNegativeButton("Close") { _, _ -> finish() }
                .show()
        } catch (_: Throwable) {
        }
    }

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

    // Wake the DisplayXR runtime package before xrCreateInstance. After a
    // force-stop / fresh install the runtime is in Android's "stopped" state,
    // so the OpenXR loader's broker lookup excludes it → XR_ERROR_RUNTIME_
    // UNAVAILABLE on a cold tap. Sending an explicit intent with
    // FLAG_INCLUDE_STOPPED_PACKAGES clears the stopped flag so the broker
    // becomes discoverable. (Test-harness convenience — real apps assume the
    // runtime was already launched once.)
    private fun wakeRuntime() {
        try {
            val intent = Intent("org.khronos.openxr.OpenXRRuntimeService").apply {
                `package` = "org.freedesktop.monado.openxr_runtime.in_process"
                addFlags(Intent.FLAG_INCLUDE_STOPPED_PACKAGES)
            }
            startService(intent)
        } catch (_: Throwable) {
            // Best-effort; the native side retries xrCreateInstance.
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        // Do NOT lock orientation — let all four orientations through.
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR
        wakeRuntime()
        super.onCreate(savedInstanceState)
        pushRotation()
        (getSystemService(Context.DISPLAY_SERVICE) as DisplayManager)
            .registerDisplayListener(displayListener, null)
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.CAMERA), REQUEST_CAMERA)
        }
        watchForRuntimeUnavailable()
        showControlsHint()
    }

    // Brief on-screen legend of the touch controls (there are no on-screen
    // buttons — the viewer is gesture-driven). Shown once per launch a couple
    // seconds in, after the model is up. A Toast sits above the weave overlay,
    // so no Vulkan HUD work is needed for a transient hint.
    private fun showControlsHint() {
        Handler(Looper.getMainLooper()).postDelayed({
            if (!isFinishing) {
                Toast.makeText(
                    this,
                    "Drag: rotate   ·   Pinch: zoom   ·   Double-tap: next model",
                    Toast.LENGTH_LONG,
                ).show()
            }
        }, 2500)
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
