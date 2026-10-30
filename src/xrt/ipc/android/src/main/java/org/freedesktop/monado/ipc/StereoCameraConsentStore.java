// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Read / revoke the stored 3D camera decisions from Java (Dashboard, adb).
 * @ingroup ipc_android
 */

package org.freedesktop.monado.ipc;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;
import androidx.annotation.NonNull;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import org.json.JSONException;
import org.json.JSONObject;

/**
 * The per-app decisions the native consent policy keeps in {@code
 * <filesDir>/displayxr/camera_consent.json} ({@code "apps": {"android:<pkg>@<cert>": "allow"|"deny"}},
 * the POSIX store format of util/u_camera_consent_store.c). Only the runtime's own processes can read
 * or write it (app-private storage). Revoking removes the entry, so the app is asked again next time.
 *
 * <p>The adb path is {@link Receiver}, exported but guarded by {@code android.permission.DUMP}, which
 * the shell holds and third-party apps cannot:
 *
 * <pre>
 * adb shell am broadcast -a org.displayxr.stereo_camera.CONSENT --es op list \
 *     -n &lt;runtime pkg&gt;/org.freedesktop.monado.ipc.StereoCameraConsentStore\$Receiver
 * ... --es op revoke --es package com.example.app     (every certificate of that package)
 * ... --es op revoke-all
 * </pre>
 */
public final class StereoCameraConsentStore {
    private static final String TAG = "dxr-camera-consent";
    public static final String FILE = "camera_consent.json";

    /** One stored decision. */
    public static final class Entry {
        public final String subject;
        public final String pkg;
        public final String decision;

        Entry(String subject, String pkg, String decision) {
            this.subject = subject;
            this.pkg = pkg;
            this.decision = decision;
        }
    }

    private StereoCameraConsentStore() {}

    private static File file(Context context) {
        return new File(StereoCameraConsent.storeDir(context), FILE);
    }

    private static synchronized JSONObject load(Context context) {
        File f = file(context);
        if (!f.exists()) {
            return new JSONObject();
        }
        try {
            return new JSONObject(new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8));
        } catch (IOException | JSONException e) {
            Log.w(TAG, "store: cannot read " + f + ": " + e);
            return null;
        }
    }

    private static synchronized boolean save(Context context, JSONObject root) {
        File f = file(context);
        File dir = f.getParentFile();
        if (dir != null && !dir.exists() && !dir.mkdirs()) {
            return false;
        }
        File tmp = new File(f.getPath() + ".tmp");
        try (FileOutputStream out = new FileOutputStream(tmp)) {
            out.write(root.toString(2).getBytes(StandardCharsets.UTF_8));
        } catch (IOException | JSONException e) {
            Log.w(TAG, "store: cannot write " + tmp + ": " + e);
            return false;
        }
        return tmp.renameTo(f);
    }

    /** Every stored per-app decision. */
    @NonNull
    public static List<Entry> list(@NonNull Context context) {
        List<Entry> out = new ArrayList<>();
        JSONObject root = load(context);
        JSONObject apps = root != null ? root.optJSONObject("apps") : null;
        if (apps == null) {
            return out;
        }
        for (Iterator<String> it = apps.keys(); it.hasNext(); ) {
            String k = it.next();
            out.add(new Entry(k, StereoCameraConsent.packageOfSubject(k), apps.optString(k, "?")));
        }
        return out;
    }

    /**
     * Forget the decision(s) for {@code pkg} (every signing certificate), or all when {@code pkg} is
     * null. Returns how many were removed; -1 if the store could not be written.
     */
    public static int revoke(@NonNull Context context, String pkg) {
        JSONObject root = load(context);
        if (root == null) {
            return -1;
        }
        JSONObject apps = root.optJSONObject("apps");
        if (apps == null) {
            return 0;
        }
        List<String> drop = new ArrayList<>();
        for (Iterator<String> it = apps.keys(); it.hasNext(); ) {
            String k = it.next();
            if (pkg == null || pkg.equals(StereoCameraConsent.packageOfSubject(k))) {
                drop.add(k);
            }
        }
        for (String k : drop) {
            apps.remove(k);
        }
        if (drop.isEmpty()) {
            return 0;
        }
        if (!save(context, root)) {
            return -1;
        }
        Log.w(TAG, "store: revoked " + drop.size() + " decision(s)" + (pkg != null ? " for " + pkg : ""));
        return drop.size();
    }

    /** The adb surface (DUMP-protected in the manifest). Result data carries the answer. */
    public static final class Receiver extends BroadcastReceiver {
        public static final String ACTION = "org.displayxr.stereo_camera.CONSENT";

        @Override
        public void onReceive(Context context, Intent intent) {
            if (!ACTION.equals(intent.getAction())) {
                return;
            }
            String op = intent.getStringExtra("op");
            StringBuilder sb = new StringBuilder();
            if (op == null || op.equals("list")) {
                for (Entry e : list(context)) {
                    sb.append(e.decision).append(' ').append(e.subject).append('\n');
                }
                if (sb.length() == 0) {
                    sb.append("(no stored decisions)");
                }
            } else if (op.equals("revoke")) {
                String pkg = intent.getStringExtra("package");
                if (pkg == null || pkg.isEmpty()) {
                    sb.append("revoke needs --es package <name>");
                } else {
                    sb.append("revoked ").append(revoke(context, pkg));
                }
            } else if (op.equals("revoke-all")) {
                sb.append("revoked ").append(revoke(context, null));
            } else {
                sb.append("unknown op ").append(op).append(" (list | revoke | revoke-all)");
            }
            Log.w(TAG, "adb consent " + op + ": " + sb.toString().trim());
            setResultCode(0);
            setResultData(sb.toString().trim());
        }
    }
}
