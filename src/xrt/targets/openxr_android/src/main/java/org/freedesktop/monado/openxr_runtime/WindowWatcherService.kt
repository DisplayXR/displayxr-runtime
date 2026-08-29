// Copyright 2026, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * Occlusion feed for the weave satellite (#1277 P1).
 *
 * An AccessibilityService is the only app-reachable source of OTHER windows'
 * on-screen rects and z-order, which the satellite needs to clip the weave
 * under overlapping 2D windows (the overlay composites above the entire app
 * window stack). On every windows-changed event this service serializes the
 * interactive window list to files/dxr_occlusion.bin — an atomic-rename file
 * in the shared app-data dir, so every runtime slot process can read it with
 * a stat+read and zero IPC plumbing.
 *
 * Not enabled by default: a user (or a dev box via adb) turns it on in
 * accessibility settings. Disabled -> the file goes stale/absent and the
 * satellite simply does no occlusion (today's behavior).
 *
 * Binary format (little-endian): u32 magic 'DXOC' (0x434f5844), u32 version=1,
 * u32 count, then per window 6 x i32: type, layer, left, top, right, bottom.
 * Bounds are AccessibilityWindowInfo.getBoundsInScreen — NOTE: for an
 * OEM-scaled freeform window these are the hybrid logical bounds clipped to
 * the panel, not the physical footprint; the native consumer corrects with
 * the same mini-window tell it uses for the client (see
 * comp_multi_weave_android.c).
 */
package org.freedesktop.monado.openxr_runtime

import android.accessibilityservice.AccessibilityService
import android.graphics.Rect
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.accessibility.AccessibilityEvent
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

class WindowWatcherService : AccessibilityService() {
    private val handler = Handler(Looper.getMainLooper())
    private val dump = Runnable { dumpWindows() }

    override fun onServiceConnected() {
        // getWindows() is empty without this flag, regardless of the XML's
        // canRetrieveWindowContent — set it programmatically too.
        serviceInfo = serviceInfo.apply {
            flags = flags or android.accessibilityservice.AccessibilityServiceInfo
                .FLAG_RETRIEVE_INTERACTIVE_WINDOWS
        }
        Log.i(TAG, "window watcher connected (weave-satellite occlusion feed)")
        dumpWindows()
    }

    override fun onAccessibilityEvent(event: AccessibilityEvent?) {
        if (event?.eventType != AccessibilityEvent.TYPE_WINDOWS_CHANGED) return
        // Debounce: window drags fire storms of change events; one dump per
        // 50 ms keeps up with gesture speed at trivial cost.
        handler.removeCallbacks(dump)
        handler.postDelayed(dump, 50)
    }

    override fun onInterrupt() {}

    override fun onDestroy() {
        // Leave no stale occlusion behind: the native side treats a missing
        // file as "no occluders".
        File(filesDir, FILE_NAME).delete()
        super.onDestroy()
    }

    private fun dumpWindows() {
        val ws = windows ?: return
        val n = minOf(ws.size, MAX_WINDOWS)
        val buf = ByteBuffer.allocate(12 + n * 24).order(ByteOrder.LITTLE_ENDIAN)
        buf.putInt(MAGIC)
        buf.putInt(VERSION)
        buf.putInt(n)
        val r = Rect()
        for (i in 0 until n) {
            val w = ws[i]
            w.getBoundsInScreen(r)
            buf.putInt(w.type)
            buf.putInt(w.layer)
            buf.putInt(r.left)
            buf.putInt(r.top)
            buf.putInt(r.right)
            buf.putInt(r.bottom)
        }
        try {
            val tmp = File(filesDir, "$FILE_NAME.tmp")
            tmp.writeBytes(buf.array())
            if (!tmp.renameTo(File(filesDir, FILE_NAME))) {
                Log.w(TAG, "occlusion file rename failed")
            }
        } catch (e: Exception) {
            Log.w(TAG, "occlusion dump failed: $e")
        }
    }

    companion object {
        private const val TAG = "DxrWindowWatcher"
        private const val FILE_NAME = "dxr_occlusion.bin"
        private const val MAGIC = 0x434f5844 // 'DXOC'
        private const val VERSION = 1
        private const val MAX_WINDOWS = 24
    }
}
