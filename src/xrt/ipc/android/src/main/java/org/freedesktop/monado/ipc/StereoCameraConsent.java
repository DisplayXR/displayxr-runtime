// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Android half of the stereo camera consent (XR_DXR_stereo_camera A2).
 * @ingroup ipc_android
 */

package org.freedesktop.monado.ipc;

import android.Manifest;
import android.app.AppOpsManager;
import android.app.KeyguardManager;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.content.pm.SigningInfo;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.PowerManager;
import android.os.Process;
import android.os.ResultReceiver;
import android.util.Log;
import androidx.annotation.Keep;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import java.io.File;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * What the runtime service's native camera manager asks the platform on Android (stereo-camera-android
 * §4). Every method is called from native (service_target.cpp) in the process that hosts the IPC server
 * — normally a satellite {@code :dxrN}, which serves exactly one client package.
 *
 * <ul>
 *   <li><b>Identity.</b> The consent subject is the peer uid's package and the SHA-256 of its current
 *       APK signing certificate ({@link #peerIdentity}). The native side builds the subject and checks
 *       it against the runtime's built-in trusted list.
 *   <li><b>OS permission.</b> The peer package must hold {@code CAMERA} and its camera app-op must not
 *       be ignored ({@link #peerCameraAllowed}); the runtime, the OS-visible camera client, must hold
 *       {@code CAMERA} too and asks the user for it the first time ({@link #ensureRuntimeCamera}).
 *   <li><b>Prompt.</b> {@link #prompt} shows {@link CameraConsentActivity} ("Allow &lt;app&gt; to use the
 *       3D camera?") and blocks for the answer.
 *   <li><b>In use.</b> {@link #inUseChanged} keeps a notification "3D camera in use by &lt;app&gt;" with a
 *       Stop action up while any stream is started.
 *   <li><b>Lock.</b> {@link #startWatching} reports screen off / keyguard to native (streams SUSPEND,
 *       resume on unlock).
 * </ul>
 *
 * Nothing here names a display vendor.
 */
@Keep
public final class StereoCameraConsent {
    private static final String TAG = "dxr-camera-consent";

    /** Answers, = enum u_camera_consent_prompt_answer. */
    public static final int ANSWER_UNAVAILABLE = 0;

    public static final int ANSWER_ALLOW = 1;
    public static final int ANSWER_ALLOW_ONCE = 2;
    public static final int ANSWER_DENY = 3;
    public static final int ANSWER_TIMEOUT = 4;

    /** Sent by the activity once it is on screen (no answer). */
    static final int RESULT_SHOWN = 100;

    /** Permission request result codes. */
    static final int RESULT_PERMISSION_GRANTED = 201;

    static final int RESULT_PERMISSION_DENIED = 202;

    static final String EXTRA_MODE = "mode";
    static final String EXTRA_PACKAGE = "package";
    static final String EXTRA_APP_NAME = "app_name";
    static final String EXTRA_RECEIVER = "receiver";
    static final String EXTRA_TIMEOUT_MS = "timeout_ms";
    static final String MODE_PROMPT = "prompt";
    static final String MODE_RUNTIME_PERMISSION = "runtime_permission";

    private static final String CHANNEL_ID = "dxr_stereo_camera";
    private static final int NOTIF_IN_USE_BASE = 0x5ca0;
    private static final int NOTIF_PROMPT_BASE = 0x5cb0;
    private static final long PROMPT_FALLBACK_MS = 2000;

    /** Serialises the runtime-permission request (one dialog at a time). */
    private static final Object permissionLock = new Object();

    private static final AtomicBoolean watching = new AtomicBoolean(false);
    private static volatile String stopAction = null;

    /*
     * Suspend state = screen locked OR consumer app not in the foreground. Measured on a retail
     * Android 12 tablet: the OS does NOT take the camera from the satellite when its client goes
     * to the background (a reopen in the background succeeded and streamed at full rate), so the
     * runtime keeps its own foreground rule. The signal is this process's own importance: a
     * satellite serves one package, bound with BIND_IMPORTANT | BIND_ABOVE_CLIENT, so it is
     * BOUND_TOP (IMPORTANCE_FOREGROUND) exactly while that package is the top app, and drops to
     * its own foreground-service level otherwise. Polled only while a stream is started.
     */
    private static final long FOREGROUND_POLL_MS = 500;

    private static volatile boolean screenLocked = false;
    private static volatile boolean consumerBackground = false;
    private static volatile boolean lastReported = false;
    private static volatile boolean polling = false;
    private static Handler watchHandler = null;

    private static synchronized void reportSuspend(String why) {
        boolean s = screenLocked || consumerBackground;
        if (s != lastReported) {
            lastReported = s;
            nativeSetLocked(s, why);
        }
    }

    private static final Runnable foregroundPoll =
            new Runnable() {
                @Override
                public void run() {
                    if (!polling) {
                        return;
                    }
                    android.app.ActivityManager.RunningAppProcessInfo info =
                            new android.app.ActivityManager.RunningAppProcessInfo();
                    android.app.ActivityManager.getMyMemoryState(info);
                    boolean bg =
                            info.importance
                                    > android.app.ActivityManager.RunningAppProcessInfo.IMPORTANCE_FOREGROUND;
                    if (bg != consumerBackground) {
                        consumerBackground = bg;
                        Log.w(
                                TAG,
                                "consumer app "
                                        + (bg ? "left the foreground" : "back in the foreground")
                                        + " (runtime process importance "
                                        + info.importance
                                        + ")");
                        reportSuspend(bg ? "consumer app not in the foreground" : "consumer app in the foreground");
                    }
                    Handler h = watchHandler;
                    if (h != null) {
                        h.postDelayed(this, FOREGROUND_POLL_MS);
                    }
                }
            };

    private static void setPolling(boolean on) {
        Handler h = watchHandler;
        if (h == null || on == polling) {
            return;
        }
        polling = on;
        if (on) {
            h.post(foregroundPoll);
        } else {
            h.removeCallbacks(foregroundPoll);
            if (consumerBackground) {
                consumerBackground = false;
                reportSuspend("no stream: foreground rule idle");
            }
        }
    }

    private StereoCameraConsent() {}

    /** The JSON store's directory (camera_consent.json), shared by every process of the runtime. */
    @Keep
    @NonNull
    public static String storeDir(@NonNull Context context) {
        return new File(context.getFilesDir(), "displayxr").getAbsolutePath();
    }

    /*
     * Identity.
     */

    /** SHA-256 of the current signing certificate(s) of {@code pkg}, lowercase hex; null if unknown. */
    @Nullable
    public static String certDigest(@NonNull PackageManager pm, @NonNull String pkg) {
        try {
            Signature[] sigs;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                PackageInfo pi = pm.getPackageInfo(pkg, PackageManager.GET_SIGNING_CERTIFICATES);
                SigningInfo si = pi.signingInfo;
                if (si == null) {
                    return null;
                }
                // Several signers: no single identity — refuse rather than pick one.
                if (si.hasMultipleSigners()) {
                    Log.w(TAG, "identity: " + pkg + " has multiple signers — not supported");
                    return null;
                }
                // The CURRENT certificate (after key rotation), not the original.
                sigs = si.getSigningCertificateHistory();
                if (sigs != null && sigs.length > 0) {
                    sigs = new Signature[] {sigs[sigs.length - 1]};
                }
            } else {
                @SuppressWarnings("deprecation")
                PackageInfo pi = pm.getPackageInfo(pkg, PackageManager.GET_SIGNATURES);
                @SuppressWarnings("deprecation")
                Signature[] old = pi.signatures;
                sigs = old;
            }
            if (sigs == null || sigs.length != 1) {
                return null;
            }
            byte[] d = MessageDigest.getInstance("SHA-256").digest(sigs[0].toByteArray());
            StringBuilder sb = new StringBuilder(64);
            for (byte b : d) {
                sb.append(String.format("%02x", b & 0xff));
            }
            return sb.toString();
        } catch (Exception e) {
            Log.w(TAG, "identity: no signing certificate for " + pkg + ": " + e);
            return null;
        }
    }

    /**
     * The consent identity of {@code uid}: {@code "<package>\t<cert sha256>"}, or null. A uid with
     * several packages (sharedUserId) is named by its shared-user name; all of them carry one
     * certificate by construction. The runtime's own uid is never a consumer identity.
     */
    @Keep
    @Nullable
    public static String peerIdentity(@NonNull Context context, int uid) {
        if (uid < 0 || uid == Process.myUid()) {
            return null;
        }
        PackageManager pm = context.getPackageManager();
        String[] pkgs = pm.getPackagesForUid(uid);
        if (pkgs == null || pkgs.length == 0) {
            Log.w(TAG, "identity: uid " + uid + " has no package");
            return null;
        }
        Arrays.sort(pkgs);
        String name = pkgs.length == 1 ? pkgs[0] : pm.getNameForUid(uid);
        if (name == null) {
            return null;
        }
        String digest = certDigest(pm, pkgs[0]);
        if (digest == null) {
            return null;
        }
        return name + "\t" + digest;
    }

    /**
     * Does {@code pkg} carry the certificate {@code sha256Hex} anywhere in its signing lineage
     * (APK Signature Scheme v3 key rotation)? Used only for a package on the built-in trusted list
     * whose CURRENT certificate is not the listed one: a rotated release key keeps its trust.
     */
    @Keep
    public static boolean hasSigningCertificate(
            @NonNull Context context, @NonNull String pkg, @NonNull String sha256Hex) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P || sha256Hex.length() != 64) {
            return false;
        }
        byte[] cert = new byte[32];
        try {
            for (int i = 0; i < 32; i++) {
                cert[i] = (byte) Integer.parseInt(sha256Hex.substring(2 * i, 2 * i + 2), 16);
            }
            return context.getPackageManager()
                    .hasSigningCertificate(pkg, cert, PackageManager.CERT_INPUT_SHA256);
        } catch (RuntimeException e) {
            return false;
        }
    }

    /** Package part of a subject "android:pkg@digest" (or the raw string). */
    @NonNull
    static String packageOfSubject(@NonNull String subject) {
        String s = subject.startsWith("android:") ? subject.substring(8) : subject;
        int at = s.lastIndexOf('@');
        return at > 0 ? s.substring(0, at) : s;
    }

    /** A user-facing label for a package (its app label), falling back to the package. */
    @NonNull
    static String labelOf(@NonNull Context context, @NonNull String pkg) {
        String p = pkg;
        int colon = p.indexOf(':');
        if (colon > 0) {
            p = p.substring(0, colon); // shared-user name: best effort
        }
        try {
            PackageManager pm = context.getPackageManager();
            ApplicationInfo ai = pm.getApplicationInfo(p, 0);
            CharSequence l = pm.getApplicationLabel(ai);
            if (l != null && l.length() > 0) {
                return l.toString();
            }
        } catch (PackageManager.NameNotFoundException e) {
            // fall through
        }
        return pkg;
    }

    /*
     * OS permission.
     */

    /**
     * Spec §7.1 step 4 on Android: the peer package holds {@code CAMERA} and its camera app-op is
     * allowed (MODE_ALLOWED or MODE_FOREGROUND — "only while using the app" is enough, because the
     * runtime's own while-in-use access follows the binding app's foreground state).
     */
    @Keep
    public static boolean peerCameraAllowed(@NonNull Context context, @NonNull String subject) {
        String pkg = packageOfSubject(subject);
        PackageManager pm = context.getPackageManager();
        try {
            if (pm.checkPermission(Manifest.permission.CAMERA, pkg) != PackageManager.PERMISSION_GRANTED) {
                Log.w(TAG, "os permission: " + pkg + " does not hold CAMERA");
                return false;
            }
            int uid = pm.getApplicationInfo(pkg, 0).uid;
            AppOpsManager ops = (AppOpsManager) context.getSystemService(Context.APP_OPS_SERVICE);
            if (ops != null) {
                int mode;
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                    mode = ops.unsafeCheckOpNoThrow(AppOpsManager.OPSTR_CAMERA, uid, pkg);
                } else {
                    @SuppressWarnings("deprecation")
                    int m = ops.checkOpNoThrow(AppOpsManager.OPSTR_CAMERA, uid, pkg);
                    mode = m;
                }
                if (mode != AppOpsManager.MODE_ALLOWED && mode != AppOpsManager.MODE_FOREGROUND) {
                    Log.w(TAG, "os permission: " + pkg + " camera app-op mode " + mode + " — denied");
                    return false;
                }
            }
            return true;
        } catch (Exception e) {
            Log.w(TAG, "os permission: cannot check " + pkg + ": " + e);
            return false;
        }
    }

    /**
     * The runtime package's own CAMERA permission (the runtime opens the camera). If missing, ask the
     * user through {@link CameraConsentActivity} and wait up to {@code timeoutMs}.
     */
    @Keep
    public static boolean ensureRuntimeCamera(@NonNull Context context, int timeoutMs) {
        if (context.checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            return true;
        }
        synchronized (permissionLock) {
            if (context.checkSelfPermission(Manifest.permission.CAMERA)
                    == PackageManager.PERMISSION_GRANTED) {
                return true;
            }
            Log.w(TAG, "runtime permission: the runtime holds no CAMERA permission — asking the user");
            int r = runActivity(context, MODE_RUNTIME_PERMISSION, null, null, timeoutMs);
            boolean ok =
                    context.checkSelfPermission(Manifest.permission.CAMERA)
                            == PackageManager.PERMISSION_GRANTED;
            Log.w(TAG, "runtime permission: request returned " + r + " -> " + (ok ? "GRANTED" : "not granted"));
            return ok;
        }
    }

    /*
     * Prompt.
     */

    /** Spec §7.1 step 9: ask the user. Blocks; returns an ANSWER_* value. */
    @Keep
    public static int prompt(
            @NonNull Context context, @NonNull String subject, @Nullable String appName, int timeoutMs) {
        String pkg = packageOfSubject(subject);
        int r = runActivity(context, MODE_PROMPT, pkg, appName, timeoutMs);
        if (r == RESULT_PERMISSION_GRANTED || r == RESULT_PERMISSION_DENIED || r == RESULT_SHOWN) {
            r = ANSWER_TIMEOUT;
        }
        Log.w(TAG, "prompt for " + pkg + " answered " + r);
        return r;
    }

    /**
     * Start {@link CameraConsentActivity} and wait for its answer. The activity is not exported; its
     * answer comes back through a {@link ResultReceiver} only this call holds, so no other app can
     * answer for the user. If the activity does not come up (background-activity-start rules), a
     * heads-up notification opens it on tap.
     */
    private static int runActivity(
            @NonNull Context context,
            @NonNull String mode,
            @Nullable String pkg,
            @Nullable String appName,
            int timeoutMs) {
        final CountDownLatch done = new CountDownLatch(1);
        final CountDownLatch shown = new CountDownLatch(1);
        final AtomicInteger answer = new AtomicInteger(ANSWER_TIMEOUT);
        HandlerThread ht = new HandlerThread("dxr-camera-consent");
        ht.start();
        ResultReceiver rr =
                new ResultReceiver(new Handler(ht.getLooper())) {
                    @Override
                    protected void onReceiveResult(int code, Bundle data) {
                        if (code == RESULT_SHOWN) {
                            shown.countDown();
                            return;
                        }
                        if (done.getCount() > 0) {
                            answer.set(code);
                            done.countDown();
                        }
                    }
                };
        Intent i = new Intent();
        i.setClassName(context.getPackageName(), CameraConsentActivity.class.getName());
        i.putExtra(EXTRA_MODE, mode);
        i.putExtra(EXTRA_PACKAGE, pkg);
        i.putExtra(EXTRA_APP_NAME, appName);
        i.putExtra(EXTRA_TIMEOUT_MS, timeoutMs);
        i.putExtra(EXTRA_RECEIVER, toParcelable(rr));
        i.addFlags(
                Intent.FLAG_ACTIVITY_NEW_TASK
                        | Intent.FLAG_ACTIVITY_MULTIPLE_TASK
                        | Intent.FLAG_ACTIVITY_EXCLUDE_FROM_RECENTS);
        NotificationManager nm = context.getSystemService(NotificationManager.class);
        int notifId = NOTIF_PROMPT_BASE + (Process.myPid() & 0xf);
        try {
            try {
                context.startActivity(i);
            } catch (RuntimeException e) {
                Log.w(TAG, "consent activity could not start directly: " + e);
            }
            if (!shown.await(PROMPT_FALLBACK_MS, TimeUnit.MILLISECONDS) && done.getCount() > 0) {
                // Not on screen: a background start was blocked. Offer it as a notification.
                Log.w(TAG, "consent activity not shown within " + PROMPT_FALLBACK_MS + " ms — posting notification");
                postPromptNotification(context, nm, notifId, i, mode, pkg);
            }
            done.await(Math.max(timeoutMs, 1000), TimeUnit.MILLISECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } finally {
            if (nm != null) {
                nm.cancel(notifId);
            }
            ht.quitSafely();
        }
        // done not counted down = no answer in time: TIMEOUT (refused, never stored). A late tap
        // finds nobody listening (the native hand-off drops it too, #1842).
        return answer.get();
    }

    /** Strip the anonymous subclass so the receiving process can unparcel it as a plain ResultReceiver. */
    private static ResultReceiver toParcelable(ResultReceiver rr) {
        android.os.Parcel p = android.os.Parcel.obtain();
        rr.writeToParcel(p, 0);
        p.setDataPosition(0);
        ResultReceiver plain = ResultReceiver.CREATOR.createFromParcel(p);
        p.recycle();
        return plain;
    }

    private static void ensureChannel(Context context, NotificationManager nm) {
        if (nm == null || Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
            return;
        }
        if (nm.getNotificationChannel(CHANNEL_ID) == null) {
            NotificationChannel ch =
                    new NotificationChannel(CHANNEL_ID, "3D camera", NotificationManager.IMPORTANCE_HIGH);
            ch.setDescription("Shown while an app uses the 3D camera, and to ask for permission");
            ch.setShowBadge(false);
            ch.enableVibration(false);
            ch.setSound(null, null);
            nm.createNotificationChannel(ch);
        }
    }

    private static void postPromptNotification(
            Context context, NotificationManager nm, int id, Intent activity, String mode, String pkg) {
        if (nm == null) {
            return;
        }
        ensureChannel(context, nm);
        PendingIntent pi =
                PendingIntent.getActivity(
                        context,
                        id,
                        activity,
                        PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        String text =
                MODE_PROMPT.equals(mode)
                        ? labelOf(context, pkg != null ? pkg : "?") + " wants to use the 3D camera"
                        : "DisplayXR needs camera access for the 3D camera";
        Notification n =
                new Notification.Builder(context, CHANNEL_ID)
                        .setSmallIcon(android.R.drawable.ic_menu_camera)
                        .setContentTitle("3D camera")
                        .setContentText(text + " — tap to answer")
                        .setContentIntent(pi)
                        .setAutoCancel(true)
                        .setCategory(Notification.CATEGORY_CALL)
                        .build();
        nm.notify(id, n);
    }

    /*
     * In-use indicator + Stop, lock watch.
     */

    /**
     * The in-use state changed: {@code streams} started streams, {@code consumers} their packages
     * (comma separated). Shows / updates / removes the "3D camera in use" notification.
     */
    @Keep
    public static void inUseChanged(@NonNull Context context, int streams, @Nullable String consumers) {
        NotificationManager nm = context.getSystemService(NotificationManager.class);
        if (nm == null) {
            return;
        }
        int id = NOTIF_IN_USE_BASE + (Process.myPid() & 0xf);
        setPolling(streams > 0);
        if (streams <= 0) {
            nm.cancel(id);
            return;
        }
        ensureChannel(context, nm);
        StringBuilder who = new StringBuilder();
        if (consumers != null) {
            for (String p : consumers.split(",")) {
                String t = p.trim();
                if (t.isEmpty()) {
                    continue;
                }
                if (who.length() > 0) {
                    who.append(", ");
                }
                who.append(labelOf(context, t));
            }
        }
        if (who.length() == 0) {
            who.append("an app");
        }
        String action = stopAction;
        Notification.Builder b =
                new Notification.Builder(context, CHANNEL_ID)
                        .setSmallIcon(android.R.drawable.ic_menu_camera)
                        .setContentTitle("3D camera in use")
                        .setContentText("Used by " + who)
                        .setOngoing(true)
                        .setOnlyAlertOnce(true)
                        .setCategory(Notification.CATEGORY_STATUS);
        if (action != null) {
            Intent stop = new Intent(action).setPackage(context.getPackageName());
            PendingIntent pi =
                    PendingIntent.getBroadcast(
                            context,
                            id,
                            stop,
                            PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
            b.addAction(new Notification.Action.Builder(null, "Stop", pi).build());
        }
        nm.notify(id, b.build());
    }

    /** Register the screen / keyguard watch and the Stop receiver for this process (idempotent). */
    @Keep
    public static void startWatching(@NonNull Context context) {
        if (!watching.compareAndSet(false, true)) {
            return;
        }
        final Context app = context.getApplicationContext() != null ? context.getApplicationContext() : context;
        stopAction = app.getPackageName() + ".STEREO_CAMERA_STOP." + Process.myPid();
        HandlerThread ht = new HandlerThread("dxr-camera-watch");
        ht.start();
        Handler h = new Handler(ht.getLooper());
        watchHandler = h;

        BroadcastReceiver lock =
                new BroadcastReceiver() {
                    @Override
                    public void onReceive(Context c, Intent intent) {
                        String a = intent.getAction();
                        if (Intent.ACTION_SCREEN_OFF.equals(a)) {
                            screenLocked = true;
                            reportSuspend("screen off");
                        } else if (Intent.ACTION_USER_PRESENT.equals(a)) {
                            screenLocked = false;
                            reportSuspend("user present (unlocked)");
                        } else if (Intent.ACTION_SCREEN_ON.equals(a)) {
                            KeyguardManager km = c.getSystemService(KeyguardManager.class);
                            if (km == null || !km.isKeyguardLocked()) {
                                screenLocked = false;
                                reportSuspend("screen on, no keyguard");
                            }
                        }
                    }
                };
        IntentFilter f = new IntentFilter();
        f.addAction(Intent.ACTION_SCREEN_OFF);
        f.addAction(Intent.ACTION_SCREEN_ON);
        f.addAction(Intent.ACTION_USER_PRESENT);
        app.registerReceiver(lock, f, null, h);

        BroadcastReceiver stop =
                new BroadcastReceiver() {
                    @Override
                    public void onReceive(Context c, Intent intent) {
                        Log.w(TAG, "Stop tapped on the 3D camera notification");
                        nativeStopAll("user Stop (notification)");
                    }
                };
        IntentFilter sf = new IntentFilter(stopAction);
        if (Build.VERSION.SDK_INT >= 33) {
            app.registerReceiver(stop, sf, null, h, Context.RECEIVER_NOT_EXPORTED);
        } else {
            // Not exported in practice: the action carries our pid, and only our own
            // PendingIntent (sent as this package) names it.
            app.registerReceiver(stop, sf, null, h);
        }

        // Initial state: a satellite created while the screen is off / locked.
        PowerManager pm = app.getSystemService(PowerManager.class);
        KeyguardManager km = app.getSystemService(KeyguardManager.class);
        boolean locked = (pm != null && !pm.isInteractive()) || (km != null && km.isKeyguardLocked());
        if (locked) {
            screenLocked = true;
            reportSuspend("locked at start");
        }
        Log.i(TAG, "watching screen lock; stop action " + stopAction);
    }

    private static native void nativeSetLocked(boolean locked, String why);

    private static native void nativeStopAll(String why);
}
