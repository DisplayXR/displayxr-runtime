// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The runtime-owned 3D camera consent dialog (XR_DXR_stereo_camera A2).
 * @ingroup ipc_android
 */

package org.freedesktop.monado.ipc;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ResultReceiver;
import android.util.Log;
import androidx.annotation.NonNull;

/**
 * Two jobs, both started only by {@link StereoCameraConsent} in the runtime's own processes (the
 * activity is NOT exported, so no other app can show it or answer it):
 *
 * <ul>
 *   <li><b>Prompt</b>: "Allow &lt;app&gt; to use the 3D camera?" naming the app label AND package —
 *       Allow / Only this time / Don't allow. Allow and Don't allow are remembered (per package and
 *       signing certificate) by the native policy; dismissing the dialog answers nothing (refused this
 *       time, not stored).
 *   <li><b>Runtime permission</b>: the runtime opens the camera on the app's behalf, so it asks for
 *       Android's CAMERA permission for itself, once.
 * </ul>
 *
 * The answer goes back through the caller's {@link ResultReceiver}; after the caller's timeout the
 * dialog closes itself, and a late tap reaches no one.
 */
public final class CameraConsentActivity extends Activity {
    private static final String TAG = "dxr-camera-consent";
    private static final int REQ_CAMERA = 0x5c;

    private ResultReceiver receiver;
    private boolean answered = false;
    private AlertDialog dialog;
    private final Handler handler = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        receiver = getIntent().getParcelableExtra(StereoCameraConsent.EXTRA_RECEIVER);
        String mode = getIntent().getStringExtra(StereoCameraConsent.EXTRA_MODE);
        int timeoutMs = getIntent().getIntExtra(StereoCameraConsent.EXTRA_TIMEOUT_MS, 60000);
        if (receiver == null || mode == null) {
            Log.w(TAG, "consent activity started without a requester — closing");
            finish();
            return;
        }
        handler.postDelayed(() -> answer(StereoCameraConsent.ANSWER_TIMEOUT), Math.max(timeoutMs, 1000));
        if (StereoCameraConsent.MODE_RUNTIME_PERMISSION.equals(mode)) {
            if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
                answer(StereoCameraConsent.RESULT_PERMISSION_GRANTED);
                return;
            }
            requestPermissions(new String[] {Manifest.permission.CAMERA}, REQ_CAMERA);
            return;
        }
        String pkg = getIntent().getStringExtra(StereoCameraConsent.EXTRA_PACKAGE);
        if (pkg == null) {
            answer(StereoCameraConsent.ANSWER_TIMEOUT);
            return;
        }
        String label = StereoCameraConsent.labelOf(this, pkg);
        String title = "Allow " + label + " to use the 3D camera?";
        String msg =
                label
                        + " ("
                        + pkg
                        + ") wants to receive the picture from this device's 3D camera.\n\n"
                        + "While the front 3D camera is in use, it is also what keeps the display "
                        + "tracking your eyes. You can change this later in the DisplayXR app.";
        int theme =
                Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q
                        ? android.R.style.Theme_DeviceDefault_Dialog_Alert
                        : android.R.style.Theme_DeviceDefault_Light_Dialog_Alert;
        dialog =
                new AlertDialog.Builder(this, theme)
                        .setTitle(title)
                        .setMessage(msg)
                        .setPositiveButton("Allow", (d, w) -> answer(StereoCameraConsent.ANSWER_ALLOW))
                        .setNeutralButton(
                                "Only this time", (d, w) -> answer(StereoCameraConsent.ANSWER_ALLOW_ONCE))
                        .setNegativeButton("Don't allow", (d, w) -> answer(StereoCameraConsent.ANSWER_DENY))
                        .setOnCancelListener(d -> answer(StereoCameraConsent.ANSWER_TIMEOUT))
                        .create();
        dialog.setCanceledOnTouchOutside(false);
        dialog.show();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (receiver != null && !answered) {
            receiver.send(StereoCameraConsent.RESULT_SHOWN, null);
        }
    }

    @Override
    public void onRequestPermissionsResult(
            int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode != REQ_CAMERA) {
            return;
        }
        boolean ok = grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED;
        answer(ok ? StereoCameraConsent.RESULT_PERMISSION_GRANTED : StereoCameraConsent.RESULT_PERMISSION_DENIED);
    }

    private void answer(int code) {
        if (answered) {
            return;
        }
        answered = true;
        handler.removeCallbacksAndMessages(null);
        if (receiver != null) {
            receiver.send(code, null);
        }
        if (dialog != null && dialog.isShowing()) {
            dialog.dismiss();
        }
        finish();
    }

    @Override
    protected void onDestroy() {
        // Closed by the system (another task, back) without an answer: not answered = refused this time.
        if (!answered && receiver != null && !isChangingConfigurations()) {
            answered = true;
            receiver.send(StereoCameraConsent.ANSWER_TIMEOUT, null);
        }
        handler.removeCallbacksAndMessages(null);
        super.onDestroy();
    }
}
