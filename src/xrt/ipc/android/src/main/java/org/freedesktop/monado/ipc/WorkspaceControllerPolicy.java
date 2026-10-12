// Copyright 2026, DisplayXR.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Android workspace-controller verification + workspace-overlay state.
 * @ingroup ipc_android
 */

package org.freedesktop.monado.ipc;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.os.Process;
import android.util.Log;
import androidx.annotation.Keep;
import androidx.annotation.NonNull;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * The Android side of the workspace overlay mode (ADR-036 D4, #1006).
 *
 * <p>Two jobs, both in the runtime's MAIN process:
 *
 * <ol>
 *   <li><b>Verification.</b> Android has no orchestrator pid and no controller binary path, so a
 *       workspace controller is a <i>package</i>: it is verified iff it declares
 *       {@code <meta-data android:name="com.displayxr.workspace_controller"
 *       android:value="true"/>} AND is signed with the runtime's own certificate ({@link
 *       PackageManager#checkSignatures}). The native IPC server asks through {@link
 *       #isVerifiedController} when a client claims the CONTROLLER class; the dev override is the
 *       native {@code debug.xrt.DXR_ALLOW_UNVERIFIED_CONTROLLER} property, applied after this
 *       check.
 *   <li><b>Overlay state.</b> The native server reports activate / deactivate / controller loss
 *       through {@link #onOverlayChanged}. While the overlay is on, {@link MonadoImpl} accepts app
 *       surfaces only from the controller's uid and the {@link SlotBroker} hands every new IPC
 *       client the main-process service, where the shared surface composites it.
 * </ol>
 */
@Keep
public final class WorkspaceControllerPolicy {
    private static final String TAG = "dxr-workspace";

    /** Manifest flag a workspace controller package declares. */
    public static final String CONTROLLER_META_DATA = "com.displayxr.workspace_controller";

    /** Notified on every overlay transition. */
    public interface Listener {
        void onOverlayChanged(boolean active, int controllerUid);
    }

    private static volatile boolean overlayActive = false;
    private static volatile int controllerUid = -1;
    private static final CopyOnWriteArrayList<Listener> listeners = new CopyOnWriteArrayList<>();

    private WorkspaceControllerPolicy() {}

    public static void addListener(@NonNull Listener l) {
        listeners.addIfAbsent(l);
    }

    public static void removeListener(@NonNull Listener l) {
        listeners.remove(l);
    }

    public static boolean isOverlayActive() {
        return overlayActive;
    }

    public static int getControllerUid() {
        return controllerUid;
    }

    /** Called from native (any thread) by the IPC server's overlay hook. */
    @Keep
    public static void onOverlayChanged(boolean active, int uid) {
        overlayActive = active;
        controllerUid = active ? uid : -1;
        Log.w(TAG, "workspace overlay " + (active ? "ON (controller uid " + uid + ")" : "OFF"));
        for (Listener l : listeners) {
            try {
                l.onOverlayChanged(active, controllerUid);
            } catch (RuntimeException e) {
                Log.e(TAG, "overlay listener threw: " + e);
            }
        }
    }

    /** Called from native on the claiming client's IPC thread. */
    @Keep
    public static boolean isVerifiedController(@NonNull Context context, int uid) {
        if (uid < 0 || uid == Process.myUid()) {
            return false;
        }
        PackageManager pm = context.getPackageManager();
        String[] pkgs = pm.getPackagesForUid(uid);
        if (pkgs == null) {
            Log.w(TAG, "verify: uid " + uid + " has no packages");
            return false;
        }
        for (String pkg : pkgs) {
            if (isVerifiedControllerPackage(context, pkg)) {
                return true;
            }
        }
        return false;
    }

    /** Meta-data AND same signing certificate as the runtime. */
    public static boolean isVerifiedControllerPackage(@NonNull Context context, String pkg) {
        if (pkg == null) {
            return false;
        }
        PackageManager pm = context.getPackageManager();
        boolean declares = false;
        try {
            ApplicationInfo ai = pm.getApplicationInfo(pkg, PackageManager.GET_META_DATA);
            declares = ai.metaData != null && ai.metaData.getBoolean(CONTROLLER_META_DATA, false);
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        }
        if (!declares) {
            return false;
        }
        int sig = pm.checkSignatures(context.getPackageName(), pkg);
        boolean ok = sig == PackageManager.SIGNATURE_MATCH;
        Log.w(
                TAG,
                "verify: "
                        + pkg
                        + " declares "
                        + CONTROLLER_META_DATA
                        + "; signature "
                        + (ok ? "MATCHES the runtime" : "does NOT match the runtime (" + sig + ")")
                        + " -> "
                        + (ok ? "VERIFIED controller" : "not verified"));
        return ok;
    }
}
