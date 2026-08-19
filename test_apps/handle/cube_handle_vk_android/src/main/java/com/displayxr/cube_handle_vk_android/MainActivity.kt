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
import android.app.AlertDialog
import android.app.NativeActivity
import android.content.Context
import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.graphics.Point
import android.hardware.display.DisplayManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Choreographer
import android.view.MotionEvent

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

    // Implemented in main.cpp. Forwards touch to the native drag-orbit / mode
    // handler. The runtime's display surface covers the NativeActivity, so the
    // native input queue never gets a touchable frame — dispatchTouchEvent on
    // this (focused) activity window is the path that actually delivers touch.
    private external fun nativeOnTouch(action: Int, x: Float, y: Float, eventTimeMs: Long)

    // Implemented in main.cpp. This window's on-screen rect + the panel extent in
    // the SAME rotation, pushed once per frame (XR_DXR_android_surface_binding /
    // runtime#1037). The native side forwards changes to the runtime with
    // xrSetAndroidWindowGeometryDXR.
    private external fun nativeSetWindowRect(
        x: Int, y: Int, w: Int, h: Int, panelW: Int, panelH: Int, displayId: Int,
    )

    // True once xrCreateInstance failed with RUNTIME_UNAVAILABLE.
    private external fun nativeRuntimeUnavailable(): Boolean

    // True once the OpenXR instance is up (runtime reached).
    private external fun nativeXrReady(): Boolean

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

    // ---------------------------------------------------------------- window rect
    //
    // Choreographer rather than a layout / position listener, because a pure
    // window MOVE produces neither: WindowFrames.didFrameSizeChange compares w/h
    // only, so the move goes out as a `oneway IWindow.moved` that updates
    // mAttachInfo.mWindowLeft/Top and nothing else — no layout, no invalidate, no
    // callback. Meanwhile SurfaceFlinger has already repositioned the layer with
    // the OLD buffer, so an un-updated weave keeps a stale interlace phase for the
    // whole drag and the per-window Kooima frustum stays anchored to the old
    // position. Cost is one getLocationOnScreen per frame plus seven int compares;
    // the native push only happens on an actual change.
    //
    // TRAP: an OEM applying OVERRIDE_SANDBOX_VIEW_BOUNDS_APIS makes
    // getLocationOnScreen return WINDOW-relative coords — every window would report
    // (0,0), silently. The opt-out is the
    // PROPERTY_COMPAT_ALLOW_SANDBOXING_VIEW_BOUNDS_APIS property in our manifest.
    private val locationOnScreen = IntArray(2)
    private var lastRect = intArrayOf(Int.MIN_VALUE, Int.MIN_VALUE, -1, -1, -1, -1, -1)
    private var rectPollRunning = false

    private val rectCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            if (!rectPollRunning) return
            sampleWindowRect()
            Choreographer.getInstance().postFrameCallback(this)
        }
    }

    private fun sampleWindowRect() {
        val view = window?.decorView ?: return
        val w = view.width
        val h = view.height
        if (w <= 0 || h <= 0) return // not laid out yet
        view.getLocationOnScreen(locationOnScreen)
        val display = view.display ?: return
        val real = Point()
        @Suppress("DEPRECATION")
        display.getRealSize(real) // the raw panel extent, not the app bounds
        val next = intArrayOf(
            locationOnScreen[0], locationOnScreen[1], w, h, real.x, real.y, display.displayId,
        )
        if (next.contentEquals(lastRect)) return
        lastRect = next
        try {
            nativeSetWindowRect(next[0], next[1], next[2], next[3], next[4], next[5], next[6])
        } catch (_: Throwable) {
            // Native lib not bound yet — the next frame retries.
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
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        pushRotation()
    }

    // Forward every touch to native (drag-orbit + double-tap mode cycle). This
    // is the activity-window path; the runtime's display overlay sits on top but
    // is FLAG_NOT_TOUCHABLE, so touches land here. Use the raw (screen) coords —
    // the activity window is fullscreen.
    override fun dispatchTouchEvent(event: MotionEvent): Boolean {
        try {
            nativeOnTouch(event.actionMasked, event.rawX, event.rawY, event.eventTime)
        } catch (_: Throwable) {
            // Native lib not bound yet — ignore until it is.
        }
        return true
    }

    override fun onResume() {
        super.onResume()
        pushRotation()
        if (!rectPollRunning) {
            // Forget the last sample so the first frame after a resume always
            // re-pushes: the surface was destroyed and rebuilt underneath us and
            // the runtime has to be told the rect again, even when it is
            // byte-identical to the one before we went away.
            lastRect = intArrayOf(Int.MIN_VALUE, Int.MIN_VALUE, -1, -1, -1, -1, -1)
            rectPollRunning = true
            Choreographer.getInstance().postFrameCallback(rectCallback)
        }
    }

    override fun onPause() {
        rectPollRunning = false
        Choreographer.getInstance().removeFrameCallback(rectCallback)
        super.onPause()
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
