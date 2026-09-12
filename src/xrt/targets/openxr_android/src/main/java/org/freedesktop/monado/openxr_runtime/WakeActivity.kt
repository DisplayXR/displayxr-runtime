// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
package org.freedesktop.monado.openxr_runtime

import android.app.Activity
import android.os.Bundle
import android.util.Log

/**
 * An activity whose only purpose is to be started, so that starting it has the
 * side effects. It draws nothing and finishes before it is ever resumed.
 *
 * WHY AN ACTIVITY AND NOT A SERVICE. A client cannot reach this runtime when the
 * runtime package is in Android's "stopped" state: a stopped package's
 * ContentProviders are unresolvable, so the Khronos loader's broker lookup fails
 * and every OpenXR app dies at xrCreateInstance (runtime#1453). The obvious fix —
 * have the client start or bind the runtime's service first — was implemented and
 * measured, and it does not work on the two OEM builds we ship against:
 *
 *  - On a Lume Phone (MyOS 13) `AutoLaunchManagerService` refuses one package
 *    starting OR binding another package's service at all: `Service RelatedStart
 *    BlockResult = true`, for startService and bindService alike, with an
 *    explicitly resolved component, from both an androidx.startup initializer and
 *    Activity.onCreate. No choice of service API escapes it.
 *  - On an NP02J the freezer freezes the idle runtime, and a bind is then refused
 *    rather than thawing it (`skip unfrozen when bringup`, `Bind failed
 *    immediately`) — runtime#1454.
 *
 * An **activity** start is subject to neither policy, and it is what a user does
 * by hand when they are told to "open the runtime app once". This activity is that
 * gesture, without the UI. Both requirements are written up as R8.6 in
 * docs/specs/vendor/oem-android-platform-requirements.md — the platform should not
 * require any of this, and this class is the workaround until it doesn't.
 *
 * WHAT IT DELIBERATELY DOES NOT DO. It does not start [
 * org.freedesktop.monado.android_common.RuntimeService]. A started service outlives
 * its clients' unbind, so starting one here would silently change the runtime
 * service's lifetime for every app on the device. Clearing the stopped flag is the
 * whole job; the loader binds the service itself, as it always did.
 */
class WakeActivity : Activity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        // Theme.NoDisplay REQUIRES finishing before onResume, or the framework
        // throws. Nothing may be added between super.onCreate and finish().
        // No caller attribution here: a FLAG_ACTIVITY_NEW_TASK start with no
        // result leaves both callingActivity and referrer null, so asking for one
        // only ever printed "unknown". The framework already logs the caller --
        // "AutoLaunchManagerService: Activity RelatedStart ... callingPkg=..." on
        // the builds where it matters.
        Log.i(TAG, "wake activity started; clearing stopped/frozen state")
        finish()
    }

    private companion object {
        const val TAG = "DisplayXRWake"
    }
}
