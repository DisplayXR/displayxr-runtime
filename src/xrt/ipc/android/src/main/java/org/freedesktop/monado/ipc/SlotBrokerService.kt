// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Main-process host for the satellite compositor-slot broker.
 * @ingroup ipc_android
 */
package org.freedesktop.monado.ipc

import android.app.Service
import android.content.Intent
import android.os.IBinder
import android.util.Log

/**
 * Hosts [SlotBroker] in the runtime APK's main process (ADR-036 D3, #1031).
 *
 * Deliberately NOT [MonadoService]: `MonadoImpl`'s constructor loads the service library and starts
 * a runtime server, so brokering through `IMonado` would spin up a full compositor — vendor plug-in
 * and all — in a process that owns no window. This service loads nothing and starts nothing; it
 * answers four binder calls and holds a table.
 *
 * It is also not a foreground service: it has no user-visible work of its own. It stays alive
 * because every client that holds a slot also holds this binding with `BIND_ABOVE_CLIENT`, so the
 * broker process inherits the importance of the apps whose assignments it is remembering.
 */
class SlotBrokerService : Service() {

    private lateinit var broker: SlotBroker

    override fun onCreate() {
        super.onCreate()
        broker = SlotBroker(this, BuildConfig.SATELLITE_SLOT_COUNT)
        Log.i(
            TAG,
            "onCreate: broker up, ${BuildConfig.SATELLITE_SLOT_COUNT} satellite slots, pid=" +
                android.os.Process.myPid(),
        )
    }

    override fun onBind(intent: Intent?): IBinder = broker

    companion object {
        private const val TAG = "dxr-slot-broker"

        /** Explicit component the client binds; kept next to the class it names. */
        const val CLASS_NAME = "org.freedesktop.monado.ipc.SlotBrokerService"
    }
}
