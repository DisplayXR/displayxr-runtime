// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// Thin NativeActivity wrapper whose only job is to surface the runtime
// CAMERA permission dialog at first launch. CNSDK's face tracker
// (libleiaSDK-faceTrackingInApp.so, loaded into this process by the
// DisplayXR runtime broker) opens the front camera during
// xrCreateSession. On Android 6+ a manifest <uses-permission> is not
// sufficient for "dangerous" perms — the user must grant at runtime.
// Without this prompt, the camera open silently fails and head
// tracking permanently uses a default centered eye position.
//
// We delegate everything else (window setup, ANativeActivity glue,
// android_main thread spawn) to NativeActivity by calling
// super.onCreate first; the permission request is async and happens
// in parallel with the native init. If the user grants in time, the
// first xrCreateSession picks up the permission. If they deny or
// haven't decided yet, CNSDK degrades to no-face state — same as
// pre-wrapper behavior, but at least the dialog appeared.

package com.displayxr.cube_handle_vk_android

import android.Manifest
import android.app.NativeActivity
import android.content.pm.PackageManager
import android.os.Bundle

class MainActivity : NativeActivity() {

    companion object {
        private const val REQUEST_CAMERA = 1
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.CAMERA), REQUEST_CAMERA)
        }
    }
}
