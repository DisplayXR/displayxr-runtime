// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
package org.freedesktop.monado.openxr_runtime

import android.app.Activity
import android.content.Intent
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
 * IT ALSO ARMS THE IPC SERVICE, and must. Clearing the stopped flag is enough for a
 * client that reaches the runtime through the Khronos loader: the loader's broker
 * query brings the service up on its own. It is NOT enough for a client that talks
 * to the IPC service directly -- the DisplayXR Browser's inline-3D connector, and
 * anything running force-ipc. On the OEM builds in question a client's bindService
 * cannot CREATE our service (AMS "Skip bringUpServiceLocked"), so such a client gets
 * no runtime socket until something else creates it. That is #1245, and
 * [DashboardActivity] already fixes it with a single startService from a foreground
 * activity. This activity exists to replace the "open the runtime app once" ritual,
 * so it has to do everything that ritual did -- otherwise it fixes the loader clients
 * and silently leaves the IPC clients broken.
 *
 * Measured on a Lume Phone: with the wake activity alone the browser's connector
 * still failed (`blockingConnect ... refused: -1`, zero ServiceRecords); after
 * DashboardActivity it connected and reported inline-3D weave ready. The only
 * difference between the two was this call.
 *
 * Note this is NOT a new "resident service" behaviour: it is the same service, with
 * the same lifetime, that a user already starts every time they open the runtime app.
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

        // Same call DashboardActivity makes, for the same reason (#1245). It must run
        // while this activity makes us a foreground app: that is what lets the service
        // be created at all on a ROM that refuses a background client's bindService.
        // MonadoService self-foregrounds with a sticky notification on first start, so
        // one start here is all it takes; later client binds attach to the live service
        // instead of being refused.
        //
        // Never let this throw past us. A wake that half-worked (stopped flag cleared,
        // service not armed) is still better than an exception propagating out of a
        // no-display activity, which surfaces to the user as the CLIENT app crashing.
        try {
            startService(
                Intent(this, org.freedesktop.monado.ipc.MonadoService::class.java)
                    .setAction(org.freedesktop.monado.ipc.BuildConfig.SERVICE_ACTION)
            )
        } catch (t: Throwable) {
            // Logged, never swallowed silently -- a silent catch is what hid the
            // broken demo wake for months.
            Log.w(TAG, "could not arm the IPC service: $t")
        }
        finish()
    }

    private companion object {
        const val TAG = "DisplayXRWake"
    }
}
