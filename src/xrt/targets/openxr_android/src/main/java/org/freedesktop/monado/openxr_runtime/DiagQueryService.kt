// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Bound service hosting the diagnostics query in the `:diag` process (#558).
 * @author David Fattal
 */
package org.freedesktop.monado.openxr_runtime

import android.app.Service
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.HandlerThread
import android.os.IBinder
import android.os.Message
import android.os.Messenger
import android.os.Process
import android.util.Log

/**
 * Runs [DiagNative.nativeRunQueryJson] in a dedicated `:diag` process
 * (android:process in the manifest) and replies with the JSON over a
 * [Messenger].
 *
 * Lifetime contract: the native query deliberately never tears the runtime
 * down — vendor plug-in destroy can hang (displayxr-leia-plugin#39). Instead
 * this service kills its own process shortly after each reply, so every
 * query gets a fresh process and a hang can never wedge the dashboard UI
 * (the activity lives in the default process).
 */
class DiagQueryService : Service() {
    companion object {
        const val MSG_RUN_QUERY = 1
        const val MSG_RESULT = 2
        const val KEY_JSON = "json"
        const val KEY_PID = "pid"
        private const val TAG = "DiagQueryService"
        private const val SELF_KILL_DELAY_MS = 250L
    }

    private lateinit var workerThread: HandlerThread
    private lateinit var workerHandler: Handler
    private lateinit var messenger: Messenger

    override fun onCreate() {
        super.onCreate()
        // The native side calls android_main_thread_dispatch_init() from
        // this thread, so it must own a Looper (HandlerThread does).
        workerThread = HandlerThread("diag-query")
        workerThread.start()
        workerHandler =
            object : Handler(workerThread.looper) {
                override fun handleMessage(msg: Message) {
                    if (msg.what != MSG_RUN_QUERY) {
                        super.handleMessage(msg)
                        return
                    }
                    val replyTo = msg.replyTo
                    val json =
                        try {
                            DiagNative.nativeRunQueryJson(this@DiagQueryService)
                        } catch (t: Throwable) {
                            Log.e(TAG, "native query failed", t)
                            "{}"
                        }
                    try {
                        val reply = Message.obtain(null, MSG_RESULT)
                        reply.data =
                            Bundle().apply {
                                putString(KEY_JSON, json)
                                putInt(KEY_PID, Process.myPid())
                            }
                        replyTo?.send(reply)
                    } catch (e: Exception) {
                        Log.e(TAG, "failed to deliver query result", e)
                    }
                    // Process death IS the runtime teardown (leia-plugin#39).
                    // Kill from a plain thread, NOT this looper: the vendor
                    // plug-in marshals work onto this thread's Looper (the
                    // native side passed it to android_main_thread_dispatch)
                    // and a blocked marshaled task would starve a postDelayed
                    // runnable forever. The sleep lets the binder reply flush.
                    Thread {
                            Thread.sleep(SELF_KILL_DELAY_MS)
                            Log.i(TAG, "diag query delivered — exiting :diag process")
                            Process.killProcess(Process.myPid())
                        }
                        .start()
                }
            }
        messenger = Messenger(workerHandler)
    }

    override fun onBind(intent: Intent?): IBinder = messenger.binder

    override fun onDestroy() {
        workerThread.quitSafely()
        super.onDestroy()
    }
}
