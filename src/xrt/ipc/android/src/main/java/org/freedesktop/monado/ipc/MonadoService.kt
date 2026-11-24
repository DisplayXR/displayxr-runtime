// Copyright 2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Service implementation for exposing IMonado.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup ipc_android
 */
package org.freedesktop.monado.ipc

import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.Looper
import android.os.Process
import android.util.Log
import dagger.hilt.android.AndroidEntryPoint
import javax.inject.Inject
import org.freedesktop.monado.auxiliary.IServiceNotification

/**
 * Implementation of a Service that provides the Monado AIDL interface.
 *
 * This is needed so that the APK can expose the binder service implemented in MonadoImpl.
 */
@AndroidEntryPoint
open class MonadoService : Service(), Watchdog.ShutdownListener, RuntimeForegroundPolicy.Listener {
    private lateinit var binder: MonadoImpl

    private lateinit var watchdog: Watchdog

    @Inject lateinit var serviceNotification: IServiceNotification

    /**
     * Which satellite compositor slot this service instance is, or `-1` for the classic single
     * main-process service (ADR-036 D3, #1031).
     *
     * The generated `MonadoServiceSlotN` subclasses, each declared with its own
     * `android:process=":dxrN"`, override this. Everything below that branches on it is about one
     * fact: a satellite hosts **exactly one** client, and must die with it.
     */
    protected open val slotIndex: Int
        get() = -1

    private val isSatellite: Boolean
        get() = slotIndex >= 0

    /**
     * Foreground-notification id, offset per slot: two `startForeground()` calls with the same id
     * would have the second process silently replace the first process's notification, and the
     * user would lose the shutdown affordance for one of the satellites.
     */
    private val notificationId: Int
        get() = serviceNotification.getNotificationId() + (if (slotIndex < 0) 0 else slotIndex + 1)

    /** Off-main-thread timer for the satellite exit backstop; see [scheduleSatelliteExit]. */
    private var exitThread: HandlerThread? = null

    /** Main-process service only: is it a foreground service right now. Main thread. */
    private var isForegroundNow = false

    /** The notification of the last start, re-used when the policy re-promotes. Main thread. */
    private var lastNotification: android.app.Notification? = null

    private val mainHandler = Handler(Looper.getMainLooper())

    override fun onCreate() {
        super.onCreate()
        Log.i(TAG, "onCreate: slot=$slotIndex pid=${Process.myPid()}")

        binder = MonadoImpl(this)
        watchdog =
            Watchdog(
                // If the surface comes from client, just stop the service when client disconnected
                // because the surface belongs to the client.
                //
                // In a satellite the client count is 0 or 1 by construction, so the shared
                // counting the watchdog was written for is trivially correct here — and onUnbind
                // below does not wait for it, it tears the process down directly.
                if (binder.canDrawOverOtherApps()) BuildConfig.WATCHDOG_TIMEOUT_MILLISECONDS else 0,
                this,
            )
        watchdog.startMonitor()

        if (isSatellite && !legacyForegroundPolicy()) {
            // A satellite is never a foreground service (see RuntimeForegroundPolicy): it lives
            // exactly as long as its one client's binding, which already carries the client's
            // importance (BIND_IMPORTANT | BIND_ABOVE_CLIENT | BIND_INCLUDE_CAPABILITIES). Being a
            // foreground service would only park the runtime uid at AppOps "fgsvc" without the
            // camera capability while the client is in the background, and delay the camera by
            // the 5 s settle on its return. No self-start either: nothing is left to foreground.
            Log.w(TAG, "onCreate: satellite slot $slotIndex runs WITHOUT a foreground service " +
                "(lives by its client's binding; debug.dxr.fgs_idle_only=0 restores the old FGS)")
            return
        }
        if (!isSatellite && !legacyForegroundPolicy()) {
            RuntimeForegroundPolicy.setListener(this, this)
        }

        // start the service so it could be foregrounded
        val intent = Intent(this, javaClass)
        intent.action = BuildConfig.SERVICE_ACTION
        startService(intent)
    }

    override fun onDestroy() {
        super.onDestroy()
        Log.d(TAG, "onDestroy")

        if (!isSatellite) {
            RuntimeForegroundPolicy.setListener(this, null)
        }
        binder.shutdown()
        watchdog.stopMonitor()
        if (isSatellite) {
            // Clean teardown finished ahead of the backstop timer — go now.
            Log.i(TAG, "onDestroy: satellite slot $slotIndex exiting (pid ${Process.myPid()})")
            Process.killProcess(Process.myPid())
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        Log.d(TAG, "onStartCommand")
        // if this isn't a restart
        if (intent != null) {
            when (intent.action) {
                BuildConfig.SERVICE_ACTION -> handleStart()
                BuildConfig.SHUTDOWN_ACTION -> handleShutdown()
            }
        }
        return START_STICKY
    }

    override fun onBind(intent: Intent): IBinder? {
        Log.d(TAG, "onBind")
        watchdog.onClientConnected()
        if (!isSatellite) {
            // A client of the main-process service itself (workspace controller, overlay
            // joiners, all-slots-busy fallback): attached, so no foreground service needed.
            RuntimeForegroundPolicy.directClient(+1, "main-process service bound")
        }
        return binder
    }

    private fun handleStart() {
        var flags = 0
        // From targeting S+, the PendingIntent needs one of FLAG_IMMUTABLE and FLAG_MUTABLE
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            flags = PendingIntent.FLAG_IMMUTABLE
        }
        // Explicit component and a per-slot request code: several services in this package
        // answer the shutdown action once the satellite slots exist, and an implicit
        // PendingIntent would always resolve to the main-process MonadoService — so the
        // notification for :dxr2 would shut down the wrong process. Distinct request codes
        // additionally keep the PendingIntents from aliasing onto one another.
        val pendingShutdownIntent =
            PendingIntent.getForegroundService(
                this,
                slotIndex + 1,
                Intent(this, javaClass).setAction(BuildConfig.SHUTDOWN_ACTION),
                flags,
            )

        val notification = serviceNotification.buildNotification(this, pendingShutdownIntent)
        lastNotification = notification

        // Always promote on a start: a start may come from startForegroundService() (the boot
        // receiver, the notification's shutdown action), which must be answered by
        // startForeground() whatever the policy says. The policy then drops it again at once
        // when a client is attached.
        promoteForeground("start")
        val attached = !RuntimeForegroundPolicy.foregroundWanted()
        if (!isSatellite && !legacyForegroundPolicy() && attached) {
            demoteForeground("start with a client attached")
        }
    }

    /** startForeground with the last notification. Main thread. False = the OS refused it. */
    private fun promoteForeground(why: String): Boolean {
        val notification = lastNotification ?: return false
        return try {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                startForeground(
                    notificationId,
                    notification,
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_MANIFEST,
                )
            } else {
                startForeground(notificationId, notification)
            }
            if (!isForegroundNow && !isSatellite) {
                Log.w(TAG, "foreground service: ON ($why)")
            }
            isForegroundNow = true
            true
        } catch (e: RuntimeException) {
            // Android 12+ ForegroundServiceStartNotAllowedException: the last client went away
            // while the runtime was in the background and the runtime is not exempt (e.g. no
            // battery-optimisation exemption). The service stays started but not foreground;
            // the next start or the next "no client" transition tries again.
            Log.w(
                TAG,
                "foreground service: could NOT be re-entered ($why): $e — the runtime stays " +
                    "started but not foreground until the next attempt",
            )
            false
        }
    }

    /** stopForeground, keeping the notification (and its shutdown action) on screen. Main thread. */
    private fun demoteForeground(why: String) {
        @Suppress("DEPRECATION")
        stopForeground(STOP_FOREGROUND_DETACH)
        if (isForegroundNow) {
            Log.w(TAG, "foreground service: OFF ($why)")
        }
        isForegroundNow = false
    }

    /** [RuntimeForegroundPolicy.Listener]: any thread; applied on the main thread. */
    override fun onForegroundWanted(foreground: Boolean, why: String) {
        mainHandler.post {
            if (foreground) {
                if (!isForegroundNow) {
                    promoteForeground("no client attached: $why")
                }
            } else if (isForegroundNow) {
                demoteForeground("client attached: $why")
            }
        }
    }

    override fun onUnbind(intent: Intent?): Boolean {
        Log.d(TAG, "onUnbind")
        watchdog.onClientDisconnected()
        if (!isSatellite) {
            RuntimeForegroundPolicy.directClient(-1, "main-process service unbound")
        }
        if (isSatellite) {
            // A satellite hosts exactly one client, so its client unbinding IS the end of this
            // process's reason to exist. Tear down rather than idle: the vendor core, the GPU
            // context, the swapchains and the panel's lens refcount all hang off this process,
            // and an idle satellite would keep its slot's memory and its vendor bindings alive
            // until the low-memory killer got round to it.
            Log.i(TAG, "onUnbind: satellite slot $slotIndex lost its client — stopping")
            scheduleSatelliteExit()
            stopSelf()
            // No onRebind: a relaunching client goes back to the broker and gets a fresh
            // process, so it can never inherit half-torn-down vendor state.
            return false
        }
        return true
    }

    /**
     * Bounded wait, then hard exit — the satellite's process must actually go away.
     *
     * `stopSelf()` only guarantees `onDestroy()`; the process itself lingers as an empty process
     * until the platform reclaims it, still holding its vendor core. Worse, `binder.shutdown()`
     * runs the display-processor teardown, which can block on a vendor core-release thread join
     * (displayxr-leia-plugin#39). This timer runs on its own thread precisely so that a hang in
     * that teardown cannot also stall the exit: whatever happens, the process is gone within the
     * grace period, and its resources with it.
     */
    private fun scheduleSatelliteExit() {
        if (exitThread != null) {
            return
        }
        val thread = HandlerThread("dxr-satellite-exit")
        thread.start()
        exitThread = thread
        Handler(thread.looper)
            .postDelayed(
                {
                    Log.i(TAG, "satellite slot $slotIndex exiting (pid ${Process.myPid()})")
                    Process.killProcess(Process.myPid())
                },
                SATELLITE_EXIT_GRACE_MS,
            )
    }

    override fun onRebind(intent: Intent?) {
        Log.d(TAG, "onRebind")
        watchdog.onClientConnected()
        if (!isSatellite) {
            RuntimeForegroundPolicy.directClient(+1, "main-process service re-bound")
        }
    }

    override fun onPrepareShutdown() {
        Log.d(TAG, "onPrepareShutdown")
    }

    override fun onShutdown() {
        Log.d(TAG, "onShutdown")
        // #1245: the MAIN-process service stays RESIDENT once started; only
        // satellites die with their client.
        //
        // Why: a client binds us with bindService(). On OEM ROMs with
        // background-start restrictions (nubia/ZTE AutoLaunchManagerService and
        // friends) the framework refuses to CREATE a service on behalf of another
        // package — "ActivityManager: Skip bringUpServiceLocked ... from callerApp
        // org.chromium.chrome". Binding a service that is ALREADY RUNNING is
        // allowed; creating one is not. So the moment we stopSelf() on idle, the
        // next client — the DisplayXR Browser's inline-3D above all — cannot get
        // us back, gets no runtime socket, and renders flat 2D with no error
        // anywhere. That is exactly the "reinstall breaks the browser" report.
        //
        // Staying resident is also what this runtime does on Windows, where
        // displayxr-service.exe is the always-on orchestrator started at logon
        // (docs/architecture/service-architecture.md). Android now matches it.
        // Between clients we are a foreground service with a notification that
        // carries a shutdown action, so the user keeps an explicit way out. While
        // a client is attached the foreground service is dropped (the notification
        // stays) — its bindings keep us alive then, and a foreground service would
        // delay the camera by 5 s after every return from the background
        // (RuntimeForegroundPolicy).
        //
        // Kill switch: `adb shell setprop debug.dxr.service_resident 0` restores
        // the old stop-when-idle behaviour.
        if (!isSatellite && residentModeEnabled()) {
            Log.i(TAG, "onShutdown: main service staying resident (#1245) — clients " +
                "cannot re-create a stopped service on background-restricted ROMs")
            return
        }
        handleShutdown()
    }

    /** #1245 kill switch — `debug.dxr.service_resident=0` opts out. */
    private fun residentModeEnabled(): Boolean = boolProperty("debug.dxr.service_resident", true)

    /**
     * Kill switch for [RuntimeForegroundPolicy]: `debug.dxr.fgs_idle_only=0` restores the old
     * behaviour (the main service AND every satellite always foreground) — read at service
     * creation, so restart the runtime after changing it.
     */
    private fun legacyForegroundPolicy(): Boolean = !boolProperty("debug.dxr.fgs_idle_only", true)

    private fun boolProperty(name: String, default: Boolean): Boolean =
        try {
            val v = Class.forName("android.os.SystemProperties")
                .getMethod("get", String::class.java, String::class.java)
                .invoke(null, name, if (default) "1" else "0") as String
            v != "0" && !v.equals("false", ignoreCase = true)
        } catch (_: Throwable) {
            default
        }

    private fun handleShutdown() {
        // The shutdown action is a startForegroundService(): answer it with startForeground()
        // even when the policy had dropped the foreground service, then stop.
        if (!isForegroundNow) {
            promoteForeground("shutdown request")
        }
        stopForeground(true)
        isForegroundNow = false
        if (!isSatellite) {
            RuntimeForegroundPolicy.serviceStoppedOnPurpose()
        }
        stopSelf()
    }

    companion object {
        private const val TAG = "MonadoService"

        /**
         * How long a satellite may take to shut down cleanly before it is killed outright.
         * Generous enough for an ordinary vendor teardown, short enough that a wedged one does
         * not keep a slot (and a vendor core) occupied while the user relaunches.
         */
        private const val SATELLITE_EXIT_GRACE_MS = 3000L
    }
}
