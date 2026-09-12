// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
package org.freedesktop.monado.openxr_runtime

import android.app.Activity
import android.os.Bundle
import android.util.Log

/** Byte-for-byte the runtime's WakeActivity behaviour, in a throwaway package. */
class WakeActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        Log.i("DisplayXRWake", "stub wake activity created")
        finish()
    }
}
