// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
package org.displayxr.client

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.os.SystemClock
import android.util.Log

/**
 * Wakes the DisplayXR runtime so the OpenXR loader can find it.
 *
 * WHY THIS EXISTS. The runtime publishes `OpenXRRuntimeBroker`, a ContentProvider
 * the Khronos loader queries to locate the active runtime. Android's FLAG_STOPPED
 * makes a stopped package's providers unresolvable, and unlike intents there is no
 * "include stopped packages" option for provider resolution. A package is stopped
 * after a fresh install until first launch, and after any force-stop — and on some
 * OEM builds closing an app from recents IS a force-stop. Every OpenXR app then
 * fails at xrCreateInstance with RUNTIME_UNAVAILABLE until a human opens the
 * runtime app by hand. See runtime#1453.
 *
 * A stopped package cannot revive itself — its providers and its broadcasts are
 * both blocked, so no runtime-side code runs. The wake has to originate in the
 * client process, which is why this is a library the client links rather than
 * anything in the runtime.
 *
 * WHY AN ACTIVITY START, NOT A SERVICE. The service version of this was written
 * and measured, and it fails on both OEM builds we ship against:
 *
 *  - Lume Phone (MyOS 13): `AutoLaunchManagerService` refuses one package starting
 *    or binding another package's service outright — `Service RelatedStart
 *    BlockResult = true` — for startService and bindService alike, with an
 *    explicitly resolved component, from both a startup initializer and
 *    Activity.onCreate. No service API escapes it.
 *  - NP02J: the freezer freezes the idle runtime and a bind is then refused rather
 *    than thawing it (runtime#1454). Only an activity start thaws a frozen process.
 *
 * So the only mechanism that works on either device is the one a user performs by
 * hand: start an activity. [WAKE_ACTIVITY] is a no-display activity in the runtime
 * that finishes immediately; starting it has the side effects and no UI.
 *
 * Both OEM behaviours are written up as R8.6 in the runtime's
 * docs/specs/vendor/oem-android-platform-requirements.md. This class is the
 * workaround, not the fix.
 */
object DisplayXrRuntime {

    private const val TAG = "DisplayXrClient"

    /** Runtime flavors, preferred first. Kept here, with the runtime, rather than
     *  copied into every app — apps were hardcoding these and would break on a rename. */
    private val RUNTIME_PACKAGES = arrayOf(
        "org.freedesktop.monado.openxr_runtime.out_of_process",
        "org.freedesktop.monado.openxr_runtime.in_process",
    )

    private const val WAKE_ACTIVITY = "org.freedesktop.monado.openxr_runtime.WakeActivity"

    /**
     * `ApplicationInfo.FLAG_STOPPED`, which is @hide. Reading a documented bit out
     * of a field we are handed is not a blocklisted API — no reflection is involved
     * — and the value has been 1 shl 21 since it was introduced. If a future
     * Android moved it, [isStopped] would read the wrong bit and we would wake
     * slightly too often or not at all; it would not crash.
     */
    private const val FLAG_STOPPED = 1 shl 21

    /** Bounded so a missing or broken runtime cannot hang app startup. */
    private const val WAKE_TIMEOUT_MS = 1500L
    private const val POLL_INTERVAL_MS = 20L

    /**
     * Makes the runtime reachable, if it isn't already.
     *
     * Cheap and side-effect-free when the runtime is already awake: it reads a
     * PackageManager flag and returns, with no activity start and so no focus
     * change. That matters — this runs on every app launch.
     *
     * @param force start the wake activity even when the package is not flagged
     *        stopped. Use this to retry after xrCreateInstance has already failed:
     *        a *frozen* runtime (runtime#1454) is not flagged stopped and is not
     *        detectable from here, so the un-forced path cannot see it.
     * @return true if the runtime should now be reachable. False means no runtime
     *         is installed, or it did not come out of the stopped state in time.
     *         Callers may proceed regardless — the loader then fails exactly as it
     *         did before, so this never makes anything worse.
     */
    @JvmOverloads
    @JvmStatic
    fun wake(context: Context, force: Boolean = false): Boolean {
        val pkg = installedRuntime(context) ?: run {
            Log.i(TAG, "no DisplayXR runtime installed; nothing to wake")
            return false
        }
        val stopped = isStopped(context, pkg)
        if (!stopped && !force) {
            Log.i(TAG, "runtime $pkg is not stopped; no wake needed")
            return true
        }
        Log.i(TAG, "waking $pkg (stopped=$stopped force=$force)")

        val intent = wakeIntent(context, pkg) ?: run {
            Log.w(TAG, "$pkg exposes no activity this client may start")
            return false
        }
        try {
            context.startActivity(intent)
        } catch (t: Throwable) {
            // Never silent. A swallowed failure here is exactly how the broken
            // service version went unnoticed in five shipped apps.
            Log.w(TAG, "wake activity start failed for $pkg: $t")
            return false
        }

        // Wait for the flag to actually clear. The start is asynchronous, and the
        // caller's next move is the loader's broker query, which fails if we race it.
        //
        // This polls a PackageManager flag rather than querying the broker: a query
        // is a synchronous binder call into the runtime process, and per R8.3 such a
        // call into a FROZEN process kills it. We must not risk killing the runtime
        // in the act of waking it.
        val deadline = SystemClock.uptimeMillis() + WAKE_TIMEOUT_MS
        while (SystemClock.uptimeMillis() < deadline) {
            if (!isStopped(context, pkg)) {
                Log.i(TAG, "runtime $pkg awake")
                return true
            }
            try {
                Thread.sleep(POLL_INTERVAL_MS)
            } catch (_: InterruptedException) {
                Thread.currentThread().interrupt()
                return false
            }
        }
        // Forced waking of an already-unstopped runtime has nothing to observe, so
        // the poll above proves nothing about it — report the start, not a state.
        if (force && !stopped) {
            Log.i(TAG, "wake activity started for $pkg (was not stopped; nothing to wait for)")
            return true
        }
        Log.w(TAG, "runtime $pkg still stopped after ${WAKE_TIMEOUT_MS}ms")
        return false
    }

    /**
     * The activity to start, preferring the runtime's dedicated no-display
     * [WAKE_ACTIVITY] and falling back to its launcher activity.
     *
     * The fallback is not defensive padding — client apps and the runtime are
     * separate APKs that update independently, so a client carrying this library
     * WILL meet runtimes older than the wake activity. Without the fallback such a
     * pairing is no better than having no library at all. With it the wake still
     * works; the user just sees the runtime's dashboard flash up, which is exactly
     * the manual workaround they are being spared, so it is strictly an
     * improvement over failing.
     */
    private fun wakeIntent(context: Context, pkg: String): Intent? {
        val flags = Intent.FLAG_ACTIVITY_NEW_TASK or
            Intent.FLAG_ACTIVITY_NO_ANIMATION or
            // Without this the start is dropped for the very package we are
            // trying to un-stop.
            Intent.FLAG_INCLUDE_STOPPED_PACKAGES

        val dedicated = Intent(Intent.ACTION_MAIN).apply {
            component = ComponentName(pkg, WAKE_ACTIVITY)
            addFlags(flags)
        }
        val resolves = try {
            context.packageManager.resolveActivity(dedicated, 0) != null
        } catch (t: Throwable) {
            Log.w(TAG, "resolveActivity failed for $pkg: $t"); false
        }
        if (resolves) {
            Log.i(TAG, "wake target: $WAKE_ACTIVITY (no-display)")
            return dedicated
        }

        val launcher = try {
            context.packageManager.getLaunchIntentForPackage(pkg)
        } catch (t: Throwable) {
            Log.w(TAG, "getLaunchIntentForPackage failed for $pkg: $t"); null
        }
        if (launcher != null) {
            Log.i(TAG, "wake target: ${pkg} launcher activity " +
                "(runtime predates the no-display wake activity; expect a visible flash)")
            launcher.addFlags(flags)
        }
        return launcher
    }

    /** True if the package is in Android's stopped state, so its providers are
     *  unresolvable. Reads a flag; touches nothing in the runtime's process. */
    private fun isStopped(context: Context, pkg: String): Boolean = try {
        val ai: ApplicationInfo = context.packageManager.getApplicationInfo(pkg, 0)
        (ai.flags and FLAG_STOPPED) != 0
    } catch (t: Throwable) {
        Log.w(TAG, "cannot read application info for $pkg: $t")
        false
    }

    private fun installedRuntime(context: Context): String? =
        RUNTIME_PACKAGES.firstOrNull {
            try {
                context.packageManager.getPackageInfo(it, 0) != null
            } catch (_: Throwable) {
                false
            }
        }
}
