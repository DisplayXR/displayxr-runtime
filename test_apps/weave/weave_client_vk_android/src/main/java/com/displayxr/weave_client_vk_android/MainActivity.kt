// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// Host Activity for the XR_DXR_weave present-owner client (#1036).
//
// Unlike the cube test apps this is NOT a NativeActivity: a present-owner needs
// a real View so it can answer "where am I on the panel?" — View.getLocationOnScreen
// is the only source of that on Android, and the runtime cannot derive it (there
// is no window handle, and a pure window MOVE raises no resize, so nothing
// downstream can observe it). We sample it on every Choreographer tick and push
// it to native, which forwards it with xrWeaveBindWindow2DXR.

package com.displayxr.weave_client_vk_android

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.view.Choreographer
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager

class MainActivity : Activity(), SurfaceHolder.Callback {

    companion object {
        private const val REQUEST_CAMERA = 1

        init {
            System.loadLibrary("weave_client_vk_android")
        }
    }

    private external fun nativeOnCreate()
    private external fun nativeOnDestroy()
    private external fun nativeSetSurface(surface: Surface?)
    private external fun nativeSetWindowGeometry(x: Int, y: Int, w: Int, h: Int, displayId: Int)
    private external fun nativeXrReady(): Boolean

    private lateinit var surfaceView: SurfaceView
    private val location = IntArray(2)
    private var lastX = Int.MIN_VALUE
    private var lastY = Int.MIN_VALUE
    private var lastW = 0
    private var lastH = 0

    // Sample the on-screen origin every frame. It is a handful of ints and the
    // native side + runtime both dedupe, so a per-frame poll is the cheapest way
    // to never miss a move (which raises no callback of its own).
    private val geometryTick = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            if (isFinishing) return
            surfaceView.getLocationOnScreen(location)
            val w = surfaceView.width
            val h = surfaceView.height
            if (w > 0 && h > 0 &&
                (location[0] != lastX || location[1] != lastY || w != lastW || h != lastH)
            ) {
                lastX = location[0]
                lastY = location[1]
                lastW = w
                lastH = h
                val displayId = if (android.os.Build.VERSION.SDK_INT >= 30) {
                    display?.displayId ?: 0
                } else {
                    @Suppress("DEPRECATION")
                    windowManager.defaultDisplay.displayId
                }
                runCatching { nativeSetWindowGeometry(location[0], location[1], w, h, displayId) }
            }
            Choreographer.getInstance().postFrameCallback(this)
        }
    }

    // The runtime service sits in Android's "stopped" state after a force-stop or
    // a fresh install, which hides its OpenXR broker from the loader. Same
    // test-harness convenience the cube app uses.
    private fun wakeRuntime() {
        for (pkg in listOf(
            "org.freedesktop.monado.openxr_runtime.out_of_process",
            "org.freedesktop.monado.openxr_runtime.in_process",
        )) {
            runCatching {
                startService(
                    Intent("org.khronos.openxr.OpenXRRuntimeService").apply {
                        `package` = pkg
                        addFlags(Intent.FLAG_INCLUDE_STOPPED_PACKAGES)
                    },
                )
            }
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        wakeRuntime()

        // Materialise the app's external files dir so the native readback dump
        // (debug.dxr.weave.dump) has somewhere to write — the framework creates
        // it lazily on first use, and `adb shell mkdir` cannot, because the dir
        // must be owned by this app's uid.
        runCatching { getExternalFilesDir(null)?.mkdirs() }

        surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(this)
        setContentView(surfaceView)

        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.CAMERA), REQUEST_CAMERA)
        }

        nativeOnCreate()
        Choreographer.getInstance().postFrameCallback(geometryTick)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        nativeSetSurface(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        nativeSetSurface(holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        nativeSetSurface(null)
    }

    override fun onDestroy() {
        Choreographer.getInstance().removeFrameCallback(geometryTick)
        runCatching { nativeOnDestroy() }
        super.onDestroy()
    }
}
