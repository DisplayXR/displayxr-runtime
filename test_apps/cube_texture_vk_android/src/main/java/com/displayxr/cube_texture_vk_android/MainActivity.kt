// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
//
// Minimal NativeActivity wrapper for the texture-class port of
// cube_texture_d3d11_win. Two jobs only:
//   1. Request CAMERA at launch (the Leia DP's face tracker needs the front
//      camera for head-tracked weaving).
//   2. Best-effort wake the DisplayXR runtime out of Android's "stopped" state
//      before xrCreateInstance (the OEM blocks one app auto-starting another;
//      a user-initiated launch of DisplayXR once is the reliable fallback).
// No HUD / rotation push / cold-start dialog / eye readout — this is a faithful
// port of the Windows texture demo, not the handle test app.

package com.displayxr.cube_texture_vk_android

import android.Manifest
import android.app.NativeActivity
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle

class MainActivity : NativeActivity() {

    companion object {
        private const val REQUEST_CAMERA = 1
        init {
            System.loadLibrary("cube_texture_vk_android")
        }
    }

    private fun wakeRuntime() {
        try {
            val intent = Intent("org.khronos.openxr.OpenXRRuntimeService").apply {
                `package` = "org.freedesktop.monado.openxr_runtime.in_process"
                addFlags(Intent.FLAG_INCLUDE_STOPPED_PACKAGES)
            }
            startService(intent)
        } catch (_: Throwable) {
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        wakeRuntime()
        super.onCreate(savedInstanceState)
        if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(arrayOf(Manifest.permission.CAMERA), REQUEST_CAMERA)
        }
    }
}
