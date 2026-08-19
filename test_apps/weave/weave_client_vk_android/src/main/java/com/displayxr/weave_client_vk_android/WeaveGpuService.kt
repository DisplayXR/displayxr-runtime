// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// #1056 — the GPU-PROCESS half of the fd-handoff shape.
//
// Runs in android:process=":gpu" and deliberately behaves the way Chromium's
// :privileged_process0 must:
//
//   * it NEVER calls bindService (it is the bindee, not a binder client),
//   * it NEVER touches org.freedesktop.monado.ipc.* — no cross-apk load of the
//     runtime's Client, no <queries> use of its own, no AIDL to MonadoService,
//   * it receives everything it needs from the other process over binder: the
//     already-connected runtime socket (ParcelFileDescriptor) and the on-screen
//     Surface it presents into (Surface is Parcelable — the same channel
//     Chromium uses for its own SurfaceWrapper).
//
// What it DOES still do — and what the spike had to establish — is load the
// OpenXR loader and let the loader pull the runtime .so out of the runtime apk.
// That part is not removable: the runtime has to be code-in-this-process.

package com.displayxr.weave_client_vk_android

import android.app.Service
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import android.os.ParcelFileDescriptor
import android.util.Log
import android.view.Surface

class WeaveGpuService : Service() {

    private external fun nativeAdoptAndStart(fd: Int, surface: Surface)

    private external fun nativeSetWindowGeometry(x: Int, y: Int, w: Int, h: Int, displayId: Int)

    private external fun nativeOnDestroy()

    // Held for the process lifetime: the native side dups, but keeping these
    // makes the ownership obvious and stops the GC closing the socket.
    private var heldFd: ParcelFileDescriptor? = null
    private var heldSurface: Surface? = null
    private var started = false

    private val handler =
        object : Handler(Looper.getMainLooper()) {
            override fun handleMessage(msg: Message) {
                when (msg.what) {
                    MSG_START -> handleStart(msg.data)
                    MSG_GEOMETRY -> handleGeometry(msg.data)
                    else -> super.handleMessage(msg)
                }
            }
        }

    private val messenger = Messenger(handler)

    override fun onBind(intent: Intent?): IBinder = messenger.binder

    private fun handleStart(data: Bundle) {
        if (started) {
            Log.w(TAG, "start: already running, ignoring")
            return
        }
        data.classLoader = javaClass.classLoader
        val pfd = data.getParcelable<ParcelFileDescriptor>(KEY_FD)
        val surface = data.getParcelable<Surface>(KEY_SURFACE)
        if (pfd == null || surface == null || !surface.isValid) {
            Log.e(TAG, "start: missing fd ($pfd) or surface ($surface)")
            return
        }
        heldFd = pfd
        heldSurface = surface
        started = true
        Log.i(
            TAG,
            "start: pid=${android.os.Process.myPid()} uid=${android.os.Process.myUid()} " +
                "adopting runtime socket fd ${pfd.fd} (#1056)",
        )
        handleGeometry(data)
        nativeAdoptAndStart(pfd.fd, surface)
    }

    private fun handleGeometry(data: Bundle) {
        val w = data.getInt(KEY_W, 0)
        val h = data.getInt(KEY_H, 0)
        if (w <= 0 || h <= 0) return
        nativeSetWindowGeometry(
            data.getInt(KEY_X, 0),
            data.getInt(KEY_Y, 0),
            w,
            h,
            data.getInt(KEY_DISPLAY, 0),
        )
    }

    override fun onDestroy() {
        runCatching { nativeOnDestroy() }
        super.onDestroy()
    }

    companion object {
        private const val TAG = "dxr-weave-gpu"

        const val MSG_START = 1
        const val MSG_GEOMETRY = 2

        const val KEY_FD = "fd"
        const val KEY_SURFACE = "surface"
        const val KEY_X = "x"
        const val KEY_Y = "y"
        const val KEY_W = "w"
        const val KEY_H = "h"
        const val KEY_DISPLAY = "display"

        init {
            System.loadLibrary("weave_client_vk_android")
        }
    }
}
