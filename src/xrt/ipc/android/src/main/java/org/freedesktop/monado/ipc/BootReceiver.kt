// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Bring the runtime service back after a reboot (#1245).
 */

package org.freedesktop.monado.ipc

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build
import android.util.Log

/**
 * Starts [MonadoService] on boot so a client's bindService() always finds a
 * LIVE service.
 *
 * Background-restricted OEM ROMs refuse to *create* a service on behalf of
 * another package (`Skip bringUpServiceLocked ... from callerApp <client>`),
 * while binding an already-running one is fine. Without this, every reboot
 * returns the device to the broken state — apps that render 3D only when they
 * can reach the runtime (the DisplayXR Browser's inline-3D most visibly) would
 * silently fall back to flat 2D until the user happened to open the DisplayXR
 * app again.
 *
 * The service self-foregrounds with its shutdown-action notification on first
 * start, so this is a foreground service start from BOOT_COMPLETED — permitted,
 * and the user retains an explicit way to stop it.
 */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val action = intent.action ?: return
        if (action != Intent.ACTION_BOOT_COMPLETED &&
            action != Intent.ACTION_LOCKED_BOOT_COMPLETED &&
            action != "android.intent.action.QUICKBOOT_POWERON"
        ) {
            return
        }
        try {
            val svc = Intent(context, MonadoService::class.java)
                .setAction(BuildConfig.SERVICE_ACTION)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(svc)
            } else {
                context.startService(svc)
            }
            Log.i(TAG, "boot: started MonadoService so client binds find it live (#1245)")
        } catch (t: Throwable) {
            // Never crash the boot broadcast — a failure here just returns the
            // device to launch-once behaviour.
            Log.w(TAG, "boot: could not start MonadoService: $t")
        }
    }

    companion object {
        private const val TAG = "MonadoBootReceiver"
    }
}
