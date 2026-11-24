// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  When the runtime package may hold a foreground service (camera app-op settle).
 * @ingroup ipc_android
 */
package org.freedesktop.monado.ipc

import android.content.Context
import android.content.Intent
import android.os.Build
import android.util.Log

/**
 * Decides when the runtime's MAIN-process [MonadoService] is a foreground service: only while
 * NO client is attached to the runtime (a satellite slot held through [SlotBroker], or a direct
 * binding of the main-process service). Satellites are never foreground services (see
 * [MonadoService.satelliteForegroundEnabled]).
 *
 * Why. AppOps keeps ONE state per uid, the most important of all the uid's processes. The
 * camera op is MODE_FOREGROUND ("while in use"): it is allowed at the foreground-service state
 * only with the camera capability, which a foreground service of type connectedDevice /
 * mediaPlayback never has. So while any runtime process is a foreground service and every client
 * is in the background, the uid rests at "fgsvc" without the camera capability. When the client
 * returns, its binding (`BIND_INCLUDE_CAPABILITIES`) brings the satellite to BOUND_TOP with the
 * camera capability, which AppOps ranks as "fg" — LESS important than "fgsvc" — so the
 * change is held for the foreground-service settle time (5 s) and the camera capability with it:
 * `openCamera` is refused with ACAMERA_ERROR_CAMERA_DISABLED for 5 s after every return from
 * HOME. Without a foreground service the uid falls to the client's background / cached state
 * instead, and coming back to "fg" is an upgrade, committed at once.
 *
 * The foreground service is still what keeps the runtime resident (#1245: background-restricted
 * ROMs cannot CREATE the service for a client, only bind a live one), but while a client is
 * attached its bindings (`BIND_IMPORTANT | BIND_ABOVE_CLIENT`) keep every runtime process alive,
 * so residency needs the foreground service only between clients.
 *
 * Main process only; all calls are cheap and thread-safe.
 */
object RuntimeForegroundPolicy {
    private const val TAG = "dxr-fgs-policy"

    /** What the policy wants the main-process service to do. */
    interface Listener {
        /** @param foreground true = be a foreground service now, false = drop it. */
        fun onForegroundWanted(foreground: Boolean, why: String)
    }

    private val lock = Any()
    private var slotClients = 0
    private var directClients = 0
    private var listener: Listener? = null
    private var appContext: Context? = null
    /** The main-process service ran in this process, so residency is expected of it. */
    private var serviceWasRunning = false

    /** True when no client is attached: the only time the runtime holds a foreground service. */
    fun foregroundWanted(): Boolean = synchronized(lock) { slotClients + directClients == 0 }

    /** Main-process [MonadoService] create (l = it) / destroy (l = null). */
    fun setListener(context: Context, l: Listener?) {
        synchronized(lock) {
            listener = l
            appContext = context.applicationContext
            if (l != null) {
                serviceWasRunning = true
            }
        }
    }

    /** The user (or the idle-stop kill switch) stopped the service: never restart it on our own. */
    fun serviceStoppedOnPurpose() {
        synchronized(lock) { serviceWasRunning = false }
    }

    /** [SlotBroker]: the number of occupied satellite slots changed. */
    fun slotsOccupied(count: Int, why: String) = update(why) { slotClients = count }

    /** Main-process [MonadoService]: a client bound (+1) or unbound (-1) it directly. */
    fun directClient(delta: Int, why: String) = update(why) {
        directClients = (directClients + delta).coerceAtLeast(0)
    }

    private inline fun update(why: String, change: () -> Unit) {
        val l: Listener?
        val before: Boolean
        val after: Boolean
        val desc: String
        synchronized(lock) {
            before = slotClients + directClients == 0
            change()
            after = slotClients + directClients == 0
            l = listener
            desc = "slots $slotClients, direct $directClients"
        }
        if (before == after) {
            return
        }
        if (l != null) {
            Log.w(
                TAG,
                if (after) {
                    "no client attached ($desc, $why) -> the runtime becomes a foreground " +
                        "service again (resident, #1245)"
                } else {
                    "client attached ($desc, $why) -> the runtime drops its foreground service " +
                        "(camera app-op settle)"
                },
            )
            l.onForegroundWanted(after, why)
            return
        }
        // The service object is gone while a client was attached (the OS stops a background
        // started service once its uid has been idle ~1 min, unless the runtime is
        // battery-optimisation exempt). Residency (#1245) wants it back: start it again. Refused
        // from the background on Android 12+ without an exemption: logged, and the next boot or
        // Dashboard launch restores it.
        if (!after) {
            return
        }
        val ctx = synchronized(lock) { if (serviceWasRunning) appContext else null } ?: return
        try {
            val svc = Intent(ctx, MonadoService::class.java).setAction(BuildConfig.SERVICE_ACTION)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                ctx.startForegroundService(svc)
            } else {
                ctx.startService(svc)
            }
            Log.w(TAG, "no client attached ($desc, $why); the service had been stopped -> restarted it")
        } catch (e: RuntimeException) {
            Log.w(
                TAG,
                "no client attached ($desc, $why); could NOT restart the stopped service from the " +
                    "background: $e (not battery-optimisation exempt?) -> resident again after " +
                    "the next boot or Dashboard launch",
            )
        }
    }
}
