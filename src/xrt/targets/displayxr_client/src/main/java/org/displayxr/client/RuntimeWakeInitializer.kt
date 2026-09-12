// Copyright 2026, The DisplayXR Project and its contributors
// SPDX-License-Identifier: Apache-2.0
package org.displayxr.client

import android.app.Activity
import android.app.Application
import android.content.Context
import android.os.Bundle
import android.util.Log
import androidx.startup.Initializer

/**
 * Wakes the runtime on the consuming app's behalf, with no code in that app — it
 * gets the behaviour by depending on this library.
 *
 * WHY IT WAKES FROM AN ACTIVITY AND NOT FROM HERE. androidx.startup runs before
 * Application.onCreate, which is the earliest useful point and was where the first
 * version of this did the wake. But the wake is now an activity start
 * ([DisplayXrRuntime]), and at initializer time the app has no window yet, so
 * Android's background-activity-launch rules can refuse it — silently, without
 * throwing. So this registers a lifecycle callback instead and wakes from the
 * first activity's onCreate.
 *
 * That is still early enough by a wide margin. Application.ActivityLifecycleCallbacks
 * .onActivityCreated is dispatched from inside `Activity.super.onCreate()`, so it
 * runs before the rest of the app's own onCreate body, and far before any native
 * xrCreateInstance.
 */
class RuntimeWakeInitializer : Initializer<Unit> {

    override fun create(context: Context) {
        val app = context.applicationContext as? Application ?: run {
            // Not fatal: an app can still call DisplayXrRuntime.wake() itself.
            Log.w(TAG, "application context unavailable; runtime will not be auto-woken")
            return
        }
        app.registerActivityLifecycleCallbacks(object : Application.ActivityLifecycleCallbacks {
            override fun onActivityCreated(activity: Activity, state: Bundle?) {
                // One wake per process. Unregister first so a wake that throws
                // cannot arm itself again on the next activity.
                app.unregisterActivityLifecycleCallbacks(this)
                DisplayXrRuntime.wake(activity)
            }

            override fun onActivityStarted(activity: Activity) {}
            override fun onActivityResumed(activity: Activity) {}
            override fun onActivityPaused(activity: Activity) {}
            override fun onActivityStopped(activity: Activity) {}
            override fun onActivitySaveInstanceState(activity: Activity, out: Bundle) {}
            override fun onActivityDestroyed(activity: Activity) {}
        })
    }

    override fun dependencies(): List<Class<out Initializer<*>>> = emptyList()

    private companion object {
        const val TAG = "DisplayXrClient"
    }
}
