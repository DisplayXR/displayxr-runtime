// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
package org.displayxr.wakeprobe

import android.app.Activity
import android.content.ComponentName
import android.content.Intent
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

        // Measure the no-display wake against an arbitrary package hosting the
        // same activity, so the property can be read without a runtime that has
        // one yet:  am start ... --es target org.displayxr.wakestub
        // CONTROL for R8.6: does a SERVICE BIND thaw a frozen runtime? The
        // shipping library no longer binds, so this lives here rather than in it.
        //   am start ... --ez bind true
        if (intent?.getBooleanExtra("bind", false) == true) {
            val probe = Intent("org.khronos.openxr.OpenXRRuntimeService")
                .apply { `package` = RUNTIME_PACKAGES[0] }
            val ri = packageManager.queryIntentServices(probe, 0).firstOrNull()
            if (ri == null) {
                Log.w(TAG, "PROBE bind control: no runtime service resolved")
            } else {
                val i = Intent("org.khronos.openxr.OpenXRRuntimeService").apply {
                    component = ComponentName(ri.serviceInfo.packageName, ri.serviceInfo.name)
                    addFlags(Intent.FLAG_INCLUDE_STOPPED_PACKAGES)
                }
                val latch = java.util.concurrent.CountDownLatch(1)
                val conn = object : android.content.ServiceConnection {
                    override fun onServiceConnected(n: ComponentName?, b: android.os.IBinder?) =
                        latch.countDown()
                    override fun onServiceDisconnected(n: ComponentName?) {}
                }
                val t3 = SystemClock.uptimeMillis()
                val accepted = bindService(i, conn, BIND_AUTO_CREATE)
                val connected = latch.await(3000, java.util.concurrent.TimeUnit.MILLISECONDS)
                Log.i(TAG, "PROBE bind control: bindService=$accepted connected=$connected " +
                    "in ${SystemClock.uptimeMillis() - t3}ms")
                try { unbindService(conn) } catch (_: Throwable) {}
            }
        }

        intent?.getStringExtra("target")?.let { target ->
            val i = Intent(Intent.ACTION_MAIN).apply {
                component = ComponentName(target, WAKE_ACTIVITY)
                addFlags(
                    Intent.FLAG_ACTIVITY_NEW_TASK or
                        Intent.FLAG_ACTIVITY_NO_ANIMATION or
                        Intent.FLAG_INCLUDE_STOPPED_PACKAGES
                )
            }
            val t2 = SystemClock.uptimeMillis()
            try {
                startActivity(i)
                Log.i(TAG, "PROBE no-display wake of $target returned in " +
                    "${SystemClock.uptimeMillis() - t2}ms")
            } catch (t: Throwable) {
                Log.w(TAG, "PROBE no-display wake of $target failed: $t")
            }
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
        const val WAKE_ACTIVITY = "org.freedesktop.monado.openxr_runtime.WakeActivity"
        val RUNTIME_PACKAGES = arrayOf(
            "org.freedesktop.monado.openxr_runtime.out_of_process",
            "org.freedesktop.monado.openxr_runtime.in_process",
        )
    }
}
