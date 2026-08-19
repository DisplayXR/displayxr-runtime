// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
//
// #1056 — the BROWSER-PROCESS half of the fd-handoff shape.
//
// This is what `displayxr-browser`'s browser process would do on Android: it has
// a Context, it can declare <queries> package visibility, and it is allowed to
// bindService — so it performs the ordinary runtime connect and then ships the
// resulting socket fd down to the process that actually renders (Chromium's GPU
// process; here WeaveGpuService in :gpu).
//
// Everything here is REFLECTIVE on purpose: `org.freedesktop.monado.ipc.Client`
// lives in the runtime apk, not in ours, exactly as it would for Chromium. The
// native runtime does the same load with loadClassFromRuntimeApk().
//
// The Client instance must be RETAINED for as long as the adopting process holds
// the session: it owns the ServiceConnection, the satellite-slot claim and the
// slot death-link token (Client.java `slotToken`). Dropping it releases the slot
// and unbinds the service out from under the adopter.

package com.displayxr.weave_client_vk_android

import android.content.Context
import android.os.ParcelFileDescriptor
import android.util.Log

class RuntimeFdConnector {

    private var client: Any? = null

    /**
     * Connect to the runtime service and return a DUP of our end of the socket.
     *
     * Blocking; must not run on the UI thread. Returns null on failure.
     */
    fun connect(context: Context): ParcelFileDescriptor? {
        for (pkg in RUNTIME_PACKAGES) {
            val pfd = runCatching { connectTo(context, pkg) }.getOrElse {
                val cause = (it as? java.lang.reflect.InvocationTargetException)?.targetException ?: it
                Log.w(TAG, "connect: $pkg failed: $cause", cause)
                null
            }
            if (pfd != null) {
                Log.i(TAG, "connect: connected via $pkg, fd ${pfd.fd}")
                return pfd
            }
        }
        return null
    }

    private fun connectTo(context: Context, pkg: String): ParcelFileDescriptor? {
        // Cross-apk class load — the browser process's job, never the GPU process's.
        val runtimeContext =
            context.createPackageContext(
                pkg,
                Context.CONTEXT_INCLUDE_CODE or Context.CONTEXT_IGNORE_SECURITY,
            )
        val clientClass = runtimeContext.classLoader.loadClass(CLIENT_CLASS)

        // Client(long nativePointer). There IS no native counterpart here — the
        // adopting process owns the native side — but NativeCounterpart rejects 0
        // ("nativePointer must not be 0"), so we pass a sentinel. Nothing
        // dereferences it: only markAsDiscardedByNative() would, and Java never
        // calls it. See #1056 — a first-class `Client.connectForHandoff(Context)`
        // would remove this wart for embedders.
        val ctor = clientClass.getConstructor(java.lang.Long.TYPE)
        val instance = ctor.newInstance(HANDOFF_SENTINEL)

        // The APPLICATION context, deliberately: Client.blockingConnect only
        // attaches a MonadoView / SystemUiController when it is handed an
        // Activity. A present-owner weaves into its own window and must not have
        // the runtime take over its surface — and the browser process would not
        // have a relevant Activity anyway.
        val blockingConnect =
            clientClass.getMethod("blockingConnect", Context::class.java, String::class.java)
        val fd = blockingConnect.invoke(instance, context.applicationContext, pkg) as Int
        if (fd < 0) {
            return null
        }

        // Client owns `fd`; take our own reference so the handoff and the Client
        // lifetime are independent.
        val field = clientClass.getDeclaredField("fd").apply { isAccessible = true }
        val theirs = field.get(instance) as ParcelFileDescriptor
        val dup = theirs.dup()

        client = instance // keep the slot claim + binding alive
        return dup
    }

    companion object {
        private const val TAG = "dxr-fd-connector"
        private const val CLIENT_CLASS = "org.freedesktop.monado.ipc.Client"

        /** Non-zero, never dereferenced. See connectTo(). */
        private const val HANDOFF_SENTINEL = 1L
        private val RUNTIME_PACKAGES =
            listOf(
                "org.freedesktop.monado.openxr_runtime.out_of_process",
                "org.freedesktop.monado.openxr_runtime.in_process",
            )
    }
}
