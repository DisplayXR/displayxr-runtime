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
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.IBinder
import android.os.Message
import android.os.Messenger
import android.os.ParcelFileDescriptor
import android.util.Log
import android.view.Choreographer
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.WindowManager

class MainActivity : Activity(), SurfaceHolder.Callback {

    companion object {
        private const val REQUEST_CAMERA = 1
        private const val TAG = "dxr-weave-main"

        /**
         * #1056: `setprop debug.dxr.fdhandoff 1` runs the fd-handoff shape instead
         * of the ordinary in-process one — this Activity connects to the runtime
         * (Java, Context, bindService, cross-apk class load) and hands the socket
         * plus its Surface to WeaveGpuService in :gpu, which owns the whole
         * OpenXR session without any of that. It is the Chromium
         * browser-process/GPU-process split, simulated.
         */
        private fun fdHandoffEnabled(): Boolean =
            runCatching {
                val get =
                    Class.forName("android.os.SystemProperties")
                        .getMethod("get", String::class.java, String::class.java)
                val v = get.invoke(null, "debug.dxr.fdhandoff", "") as String
                v == "1" || v == "true"
            }.getOrDefault(false)

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
                val displayId = currentDisplayId()
                if (handoff) {
                    val data = Bundle().apply {
                        putInt(WeaveGpuService.KEY_X, location[0])
                        putInt(WeaveGpuService.KEY_Y, location[1])
                        putInt(WeaveGpuService.KEY_W, w)
                        putInt(WeaveGpuService.KEY_H, h)
                        putInt(WeaveGpuService.KEY_DISPLAY, displayId)
                    }
                    runCatching {
                        gpuMessenger?.send(
                            Message.obtain(null, WeaveGpuService.MSG_GEOMETRY).also { it.data = data },
                        )
                    }
                    maybeHandOff()
                } else {
                    runCatching { nativeSetWindowGeometry(location[0], location[1], w, h, displayId) }
                }
            }
            Choreographer.getInstance().postFrameCallback(this)
        }
    }

    // ---- #1056 fd-handoff mode ----
    private val handoff = fdHandoffEnabled()
    private var gpuMessenger: Messenger? = null
    private var runtimeFd: ParcelFileDescriptor? = null
    private var handoffStarted = false
    private val connector = RuntimeFdConnector()

    private val gpuConnection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, binder: IBinder?) {
            gpuMessenger = Messenger(binder)
            Log.i(TAG, "handoff: :gpu bound")
            maybeHandOff()
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            gpuMessenger = null
            handoffStarted = false
        }
    }

    /**
     * Connect to the runtime on a worker thread (blockingConnect must not run on
     * the UI thread) and bind the :gpu process. Whichever finishes last triggers
     * the handoff.
     */
    private fun startHandoff() {
        bindService(
            Intent(this, WeaveGpuService::class.java),
            gpuConnection,
            Context.BIND_AUTO_CREATE or Context.BIND_IMPORTANT,
        )
        Thread {
            val t0 = System.nanoTime()
            val pfd = connector.connect(this)
            val ms = (System.nanoTime() - t0) / 1e6
            if (pfd == null) {
                Log.e(TAG, "handoff: runtime connect FAILED")
                return@Thread
            }
            Log.i(TAG, "HANDOFF: browser-side connect took %.1f ms, fd %d".format(ms, pfd.fd))
            runOnUiThread {
                runtimeFd = pfd
                maybeHandOff()
            }
        }.start()
    }

    private fun maybeHandOff() {
        if (handoffStarted) return
        val messenger = gpuMessenger ?: return
        val pfd = runtimeFd ?: return
        val surface = surfaceView.holder.surface
        if (surface == null || !surface.isValid || surfaceView.width <= 0) return

        handoffStarted = true
        surfaceView.getLocationOnScreen(location)
        val data = Bundle().apply {
            putParcelable(WeaveGpuService.KEY_FD, pfd)
            putParcelable(WeaveGpuService.KEY_SURFACE, surface)
            putInt(WeaveGpuService.KEY_X, location[0])
            putInt(WeaveGpuService.KEY_Y, location[1])
            putInt(WeaveGpuService.KEY_W, surfaceView.width)
            putInt(WeaveGpuService.KEY_H, surfaceView.height)
            putInt(WeaveGpuService.KEY_DISPLAY, currentDisplayId())
        }
        Log.i(TAG, "HANDOFF: sending fd + Surface to :gpu (pid ${android.os.Process.myPid()} -> :gpu)")
        messenger.send(Message.obtain(null, WeaveGpuService.MSG_START).also { it.data = data })
    }

    private fun currentDisplayId(): Int =
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            display?.displayId ?: 0
        } else {
            @Suppress("DEPRECATION")
            windowManager.defaultDisplay.displayId
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

        if (handoff) {
            Log.w(TAG, "debug.dxr.fdhandoff=1 — running the #1056 fd-handoff shape; this process does NOT create an OpenXR instance")
            startHandoff()
        } else {
            nativeOnCreate()
        }
        Choreographer.getInstance().postFrameCallback(geometryTick)
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        if (handoff) maybeHandOff() else nativeSetSurface(holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        if (handoff) maybeHandOff() else nativeSetSurface(holder.surface)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        if (!handoff) nativeSetSurface(null)
    }

    override fun onDestroy() {
        Choreographer.getInstance().removeFrameCallback(geometryTick)
        if (handoff) {
            runCatching { unbindService(gpuConnection) }
        } else {
            runCatching { nativeOnDestroy() }
        }
        super.onDestroy()
    }
}
