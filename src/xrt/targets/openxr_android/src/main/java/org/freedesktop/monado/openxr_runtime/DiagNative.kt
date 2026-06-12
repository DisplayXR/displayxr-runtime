// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Kotlin face of the displayxr-diag JNI bridge (#558).
 * @author David Fattal
 */
package org.freedesktop.monado.openxr_runtime

import android.content.Context

/**
 * Loads the diag query bridge (targets/diag-lib). Only ever used from the
 * short-lived `:diag` process — see [DiagQueryService] for the lifetime
 * contract (the native side deliberately skips runtime teardown).
 */
object DiagNative {
    init {
        System.loadLibrary("displayxr-diag")
    }

    /**
     * Runs the headless `displayxr-cli info`/`selftest` query (real plug-in
     * discovery, no compositor) plus, on the out-of-process flavor with the
     * service up, a session-less IPC monitor pass. Returns one JSON string:
     * `{"live": …|null, "info": …, "selftest": …}`. Blocking — call from a
     * Looper-bearing worker thread (vendor DP init marshals onto it).
     */
    external fun nativeRunQueryJson(context: Context): String
}
