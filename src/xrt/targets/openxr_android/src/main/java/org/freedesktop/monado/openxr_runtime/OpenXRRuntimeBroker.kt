// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
package org.freedesktop.monado.openxr_runtime

import android.content.ContentProvider
import android.content.ContentValues
import android.database.Cursor
import android.database.MatrixCursor
import android.net.Uri
import android.util.Log

/**
 * Runtime broker ContentProvider.
 *
 * The Khronos OpenXR loader (`openxr_loader_for_android`) discovers
 * the active runtime by querying a ContentProvider at one of two
 * authorities:
 *   - `org.khronos.openxr.runtime_broker`        — installable
 *     broker, per-user. THIS class.
 *   - `org.khronos.openxr.system_runtime_broker` — system-installed
 *     broker. Reserved for OS images that pre-select a vendor
 *     runtime; we can't ship one because we're not a system app.
 *
 * URI shape the loader queries:
 *   content://<authority>/openxr/<MAJOR>/abi/<ABI>/runtimes/active
 *
 * The loader fills MAJOR (e.g. "1") and ABI (e.g. "arm64-v8a") at
 * query time. We always return one row pointing at our own runtime
 * `.so` regardless of which ABI is asked for — Android Package
 * Manager already filtered installs to compatible ABIs by the time
 * this APK is on-device.
 *
 * No active-runtime selection logic (SharedPreferences, system
 * setting, etc.) because the DisplayXR runtime APK is the only thing
 * this broker advertises. If multiple OpenXR runtimes coexist on a
 * device, a vendor / OEM is expected to ship a separate broker APK
 * that knows the local selection rules and shadows this one.
 *
 * Function-remapping ("has_functions=1") path is unused — the
 * runtime exports the standard `xrNegotiateLoaderRuntimeInterface`
 * entry point, no custom symbol mapping needed.
 */
class OpenXRRuntimeBroker : ContentProvider() {

    override fun onCreate(): Boolean = true

    override fun query(
        uri: Uri,
        projection: Array<String>?,
        selection: String?,
        selectionArgs: Array<String>?,
        sortOrder: String?,
    ): Cursor? {
        val segs = uri.pathSegments
        // Active-runtime query:
        //   /openxr/<MAJOR>/abi/<ABI>/runtimes/active
        if (segs.size != 6 ||
            segs[0] != "openxr" ||
            segs[2] != "abi" ||
            segs[4] != "runtimes" ||
            segs[5] != "active"
        ) {
            Log.w(TAG, "Unrecognized broker URI: $uri")
            return null
        }
        val major = segs[1].toIntOrNull() ?: return null
        if (major != 1) {
            // Only OpenXR 1.x is supported by this runtime. Returning
            // null here lets the loader fall back to the system
            // authority cleanly.
            Log.i(TAG, "OpenXR major=$major not supported by DisplayXR (only 1.x)")
            return null
        }

        val ctx = context ?: return null
        val info = ctx.applicationInfo

        val cursor = MatrixCursor(COLUMNS)
        cursor.addRow(
            arrayOf(
                ctx.packageName,
                // ABI-specific dir: /data/app/<pkg>-<hash>/lib/<ABI>/.
                // applicationInfo.nativeLibraryDir already returns the
                // ABI-suffixed path matched at install time. The loader
                // appends the so_filename and dlopens, so this is the
                // path we want regardless of which ABI the loader
                // requested (Android filtered at install).
                info.nativeLibraryDir,
                // CMake sets PREFIX "" on the runtime target, so the
                // file on disk is `openxr_displayxr.so` (not the
                // Android-default `libopenxr_displayxr.so`). Match
                // exactly what shipped in the APK's jniLibs/.
                "openxr_displayxr.so",
                // No custom function mappings.
                0,
            ),
        )
        Log.i(TAG, "Active runtime resolved: ${ctx.packageName}/openxr_displayxr.so in ${info.nativeLibraryDir}")
        return cursor
    }

    override fun getType(uri: Uri): String? = null

    override fun insert(uri: Uri, values: ContentValues?): Uri? = null

    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<String>?): Int = 0

    override fun update(
        uri: Uri,
        values: ContentValues?,
        selection: String?,
        selectionArgs: Array<String>?,
    ): Int = 0

    companion object {
        private const val TAG = "DisplayXR-Broker"

        private val COLUMNS =
            arrayOf("package_name", "native_lib_dir", "so_filename", "has_functions")
    }
}
