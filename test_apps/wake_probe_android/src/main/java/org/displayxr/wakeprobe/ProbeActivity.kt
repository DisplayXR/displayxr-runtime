// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
package org.displayxr.wakeprobe

import android.app.Activity
import android.content.pm.ApplicationInfo
import android.os.Bundle
import android.os.SystemClock
import android.util.Log
import org.displayxr.client.DisplayXrRuntime

/**
 * Measurement harness for the runtime wake (runtime#1453 / #1454).
 *
 * By the time this body runs, the library's initializer has already woken the
 * runtime from the lifecycle callback dispatched inside `super.onCreate()`. So
 * this reports the state the automatic path left behind, rather than doing the
 * wake itself — a probe that wakes in its own onCreate proves the API works but
 * not that the shipping path fires early enough.
 *
 *   adb shell am start -n org.displayxr.wakeprobe/.ProbeActivity
 *   adb shell am start -n org.displayxr.wakeprobe/.ProbeActivity --ez force true
 */
class ProbeActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        val t0 = SystemClock.uptimeMillis()
        super.onCreate(savedInstanceState)          // <- the automatic wake happens in here
        val autoMs = SystemClock.uptimeMillis() - t0

        Log.i(TAG, "PROBE after-auto-wake stopped=${stoppedFlags()} autoWakeCostMs=$autoMs")

        if (intent?.getBooleanExtra("force", false) == true) {
            val t1 = SystemClock.uptimeMillis()
            val ok = DisplayXrRuntime.wake(this, force = true)
            Log.i(TAG, "PROBE forced wake -> $ok in ${SystemClock.uptimeMillis() - t1}ms")
        }
    }

    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        // The open question about an activity-based wake is whether it steals
        // focus. If the no-display wake activity is doing its job, this prints
        // exactly one line, hasFocus=true, and never a false/true pair.
        Log.i(TAG, "PROBE focus=$hasFocus")
    }

    /** The stopped bit for each runtime flavor, read the same way the library does. */
    private fun stoppedFlags(): String = RUNTIME_PACKAGES.joinToString(" ") { pkg ->
        val s = try {
            val ai: ApplicationInfo = packageManager.getApplicationInfo(pkg, 0)
            if ((ai.flags and FLAG_STOPPED) != 0) "STOPPED" else "ok"
        } catch (_: Throwable) {
            "absent"
        }
        "${pkg.substringAfterLast('.')}=$s"
    }

    private companion object {
        const val TAG = "DisplayXrClient"
        const val FLAG_STOPPED = 1 shl 21
        val RUNTIME_PACKAGES = arrayOf(
            "org.freedesktop.monado.openxr_runtime.out_of_process",
            "org.freedesktop.monado.openxr_runtime.in_process",
        )
    }
}
