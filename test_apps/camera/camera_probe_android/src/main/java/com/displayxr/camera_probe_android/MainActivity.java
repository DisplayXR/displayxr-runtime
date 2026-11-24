// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
package com.displayxr.camera_probe_android;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;
import android.widget.TextView;

/**
 * XR_DXR_stereo_camera consumer probe (the Android stand-in for
 * `displayxr-cli camera probe`). A CAMERA_CONSUMER-only XrInstance: no session,
 * no rendering. Extras: camera = front|rear|<index> (default front), secs (20),
 * output = rectified|raw (rectified). Results go to logcat (tag
 * dxr_camera_probe) and the last frame to files/cam_last.nv12.
 *
 * Spec v8 extra (one string, ';'-separated, all optional): v8 =
 *   mode=WxH|max      the stream's capture mode (default: the camera's default)
 *   focus=auto|once|manual:<dioptres>|locked   point=x,y (left eye, 0..1)   ev=<EV>
 *   still=WxH|max     one still halfway through, written to files/still_last.<fmt>
 * e.g. adb shell am start -n com.displayxr.camera_probe_android/.MainActivity
 *        --es camera rear --es v8 "mode=max;focus=once;point=0.5,0.5;still=max"
 * Modes and control capabilities are always listed.
 */
public class MainActivity extends Activity {
    static { System.loadLibrary("camera_probe_android"); }

    private static native String nativeRun(Activity activity, String camera, int secs, String output, String outPath,
                                           String v8);

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        final TextView tv = new TextView(this);
        tv.setTextSize(20);
        tv.setText("DisplayXR camera probe: running… (logcat tag dxr_camera_probe)");
        setContentView(tv);
        final String camera = getIntent().hasExtra("camera") ? getIntent().getStringExtra("camera") : "front";
        final int secs = getIntent().getIntExtra("secs", 20);
        final String output = getIntent().hasExtra("output") ? getIntent().getStringExtra("output") : "rectified";
        final String out = getFilesDir() + "/cam_last.nv12";
        final String v8 = getIntent().hasExtra("v8") ? getIntent().getStringExtra("v8") : "";
        new Thread(() -> {
            final String r = nativeRun(this, camera, secs, output, out, v8);
            Log.w("dxr_camera_probe", "RESULT " + r);
            runOnUiThread(() -> tv.setText("DisplayXR camera probe:\n" + r));
        }, "camera-probe").start();
    }
}
