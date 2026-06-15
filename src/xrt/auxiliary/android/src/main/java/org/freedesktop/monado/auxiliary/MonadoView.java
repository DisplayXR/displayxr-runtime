// Copyright 2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Class to inject a custom surface into an activity.
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup aux_android_java
 */

package org.freedesktop.monado.auxiliary;

import android.app.Activity;
import android.content.Context;
import android.graphics.PixelFormat;
import android.graphics.Region;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.ViewTreeObserver;
import android.view.WindowManager;
import androidx.annotation.GuardedBy;
import androidx.annotation.Keep;
import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import java.util.TreeSet;

@Keep
public class MonadoView extends SurfaceView
        implements SurfaceHolder.Callback, SurfaceHolder.Callback2 {
    private static final String TAG = "MonadoView";

    /**
     * Observer for the surface lifecycle, fired from the SurfaceHolder callbacks on the UI thread.
     *
     * <p>Used by the out-of-process IPC client to forward surface destroy/recreate across the
     * binder boundary so the service compositor can rebuild its VkSurfaceKHR (#528). Pure Java by
     * design — JNI callback registration proved unreliable across classloaders (#507).
     */
    @Keep
    public interface SurfaceStateListener {
        /** A live surface is available (surfaceCreated/surfaceChanged). UI thread. */
        void onSurfaceAvailable(@NonNull SurfaceHolder holder);

        /** The current surface was destroyed. UI thread. */
        void onSurfaceDestroyed();
    }

    @Nullable private SurfaceStateListener surfaceStateListener = null;

    private final Object currentSurfaceHolderSync = new Object();

    public int width = -1;
    public int height = -1;
    public int format = -1;

    private NativeCounterpart nativeCounterpart;

    @GuardedBy("currentSurfaceHolderSync") @Nullable private SurfaceHolder currentSurfaceHolder = null;

    private SystemUiController systemUiController = null;

    // Host activity (when attached to one), so touch on this overlay can be
    // forwarded to the app. The overlay covers the app's own window, so it's
    // the only window that receives touch — forwarding lets an in-process app
    // handle input via Activity.dispatchTouchEvent (#499).
    @Nullable private Activity hostActivity = null;

    // #558 P3: when this is a SERVICE-owned overlay (no host Activity) it floats
    // over the live launcher and must pass touches through WITHOUT
    // FLAG_NOT_TOUCHABLE (that flag makes Android clamp the window to <=0.80
    // alpha, which blends away the LeiaSR interlace → the weave looks 2D).
    // Instead we declare an explicit touchable Region (empty = full passthrough,
    // full opacity). Guarded; a non-empty region (the tiger silhouette) can be
    // set later to make just the tiger touchable.
    private final Object touchableRegionSync = new Object();

    @GuardedBy("touchableRegionSync")
    private final Region touchableRegion = new Region(); // empty == pass everything through

    private boolean passthroughInstalled = false;

    public MonadoView(Context context) {
        super(context);

        if (context instanceof Activity) {
            Activity activity = (Activity) context;
            hostActivity = activity;
            systemUiController = new SystemUiController(activity.getWindow().getDecorView());
            systemUiController.hide();
        }
        SurfaceHolder surfaceHolder = getHolder();
        surfaceHolder.addCallback(this);
        // A translucent SurfaceView z-ordered as a media overlay lets SurfaceFlinger
        // composite the weaved content over the live screen (#568). This view is only
        // created as a SERVICE-owned overlay (no host Activity) for a per-app overlay
        // (see-through) app (#558), so make it translucent in that case; debug.dxr.transparent
        // stays as a dev force override for the in-Activity path.
        if (hostActivity == null || isTransparentSpikeEnabled()) {
            setZOrderMediaOverlay(true);
            surfaceHolder.setFormat(PixelFormat.TRANSLUCENT);
            Log.i(TAG, "MonadoView: TRANSLUCENT media-overlay surface (#558 overlay / #568)");
        }
    }

    /** Reads the `debug.dxr.transparent` sysprop via the hidden SystemProperties API. */
    private static boolean isTransparentSpikeEnabled() {
        try {
            Class<?> sp = Class.forName("android.os.SystemProperties");
            String v = (String) sp.getMethod("get", String.class).invoke(null, "debug.dxr.transparent");
            return v != null && (v.startsWith("1") || v.startsWith("t") || v.startsWith("T")
                    || v.startsWith("y") || v.startsWith("Y"));
        } catch (Exception e) {
            return false;
        }
    }

    @Override
    public boolean onTouchEvent(android.view.MotionEvent event) {
        // Forward to the host activity so the app can handle drag/tap input.
        // The activity's dispatchTouchEvent goes to its own view hierarchy
        // (not back to this separate overlay window), so there's no loop.
        if (hostActivity != null) {
            hostActivity.dispatchTouchEvent(event);
            return true; // claim the gesture so we keep receiving MOVE/UP
        }
        return super.onTouchEvent(event);
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        // Service-owned overlay (no host Activity): install the passthrough touch
        // region so the live launcher below stays interactive. In-process
        // (host Activity present) keeps the full-touchable forward-to-Activity
        // behavior (#499) untouched.
        if (hostActivity == null) {
            installPassthroughTouchRegion();
        }
    }

    /**
     * #558 P3: make this service-owned overlay declare an explicit touchable Region via the hidden
     * ViewTreeObserver.addOnComputeInternalInsetsListener API, instead of FLAG_NOT_TOUCHABLE.
     *
     * <p>FLAG_NOT_TOUCHABLE would make Android clamp this system overlay to &lt;=0.80 window alpha
     * (anti-tapjacking), and an 0.80 whole-window blend destroys the LeiaSR per-pixel interlace so
     * the weave looks 2D. A touchable Region carries no such clamp: an EMPTY region means nothing
     * here is touchable (everything passes through to the launcher) while the window stays fully
     * opaque so the 3D weave survives. Reflected because the API is {@code @hide} (this file already
     * relies on reflection for SystemProperties).
     */
    private static boolean sHiddenApiExempted = false;

    /**
     * Lift Android's non-SDK (hidden-API) blocklist for this process so the touch-region reflection
     * below can see {@code ViewTreeObserver$InternalInsetsInfo.touchableRegion} (blocklisted on
     * Android 14 → getField throws NoSuchFieldException). Uses the well-known double-reflection
     * bootstrap (Class.forName + getDeclaredMethod obtained reflectively bypass the caller check)
     * to call VMRuntime.setHiddenApiExemptions("L"). Same technique Shizuku/LSPosed use.
     */
    private static void exemptHiddenApis() {
        if (sHiddenApiExempted) {
            return;
        }
        sHiddenApiExempted = true;
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) {
            return; // no non-SDK restrictions before Android 9
        }
        try {
            java.lang.reflect.Method forName =
                    Class.class.getDeclaredMethod("forName", String.class);
            java.lang.reflect.Method getDeclaredMethod =
                    Class.class.getDeclaredMethod(
                            "getDeclaredMethod", String.class, Class[].class);
            Class<?> vmRuntimeClass = (Class<?>) forName.invoke(null, "dalvik.system.VMRuntime");
            java.lang.reflect.Method getRuntime =
                    (java.lang.reflect.Method)
                            getDeclaredMethod.invoke(vmRuntimeClass, "getRuntime", (Class<?>[]) null);
            java.lang.reflect.Method setHiddenApiExemptions =
                    (java.lang.reflect.Method)
                            getDeclaredMethod.invoke(
                                    vmRuntimeClass,
                                    "setHiddenApiExemptions",
                                    new Class[] {String[].class});
            Object vmRuntime = getRuntime.invoke(null);
            setHiddenApiExemptions.invoke(vmRuntime, new Object[] {new String[] {"L"}});
            Log.i(TAG, "MonadoView: hidden-API exemptions applied (touch-region reflection)");
        } catch (Throwable t) {
            Throwable cause = (t instanceof java.lang.reflect.InvocationTargetException
                            && t.getCause() != null)
                    ? t.getCause()
                    : t;
            Log.w(TAG, "MonadoView: hidden-API exemption failed: " + cause, cause);
        }
    }

    private void installPassthroughTouchRegion() {
        if (passthroughInstalled) {
            return;
        }
        exemptHiddenApis();
        try {
            final Class<?> infoCls =
                    Class.forName("android.view.ViewTreeObserver$InternalInsetsInfo");
            final Class<?> listenerCls =
                    Class.forName("android.view.ViewTreeObserver$OnComputeInternalInsetsListener");
            final java.lang.reflect.Method setTouchableInsets =
                    infoCls.getMethod("setTouchableInsets", int.class);
            final java.lang.reflect.Field touchableRegionField =
                    infoCls.getField("touchableRegion");
            final int touchableInsetsRegion =
                    infoCls.getField("TOUCHABLE_INSETS_REGION").getInt(null);

            final Object listener =
                    java.lang.reflect.Proxy.newProxyInstance(
                            listenerCls.getClassLoader(),
                            new Class<?>[] {listenerCls},
                            (proxy, method, args) -> {
                                String name = method.getName();
                                if ("onComputeInternalInsets".equals(name)
                                        && args != null
                                        && args.length == 1) {
                                    Object info = args[0];
                                    setTouchableInsets.invoke(info, touchableInsetsRegion);
                                    Region region = (Region) touchableRegionField.get(info);
                                    if (region != null) {
                                        synchronized (touchableRegionSync) {
                                            region.set(touchableRegion);
                                        }
                                    }
                                    return null;
                                }
                                if ("hashCode".equals(name)) {
                                    return System.identityHashCode(proxy);
                                }
                                if ("equals".equals(name)) {
                                    return proxy == (args != null ? args[0] : null);
                                }
                                if ("toString".equals(name)) {
                                    return "MonadoViewPassthroughInsetsListener";
                                }
                                return null;
                            });

            final java.lang.reflect.Method addListener =
                    ViewTreeObserver.class.getMethod(
                            "addOnComputeInternalInsetsListener", listenerCls);
            addListener.invoke(getViewTreeObserver(), listener);
            passthroughInstalled = true;
            Log.i(TAG, "MonadoView: passthrough touch region installed (#558 P3)");
        } catch (Throwable t) {
            Log.w(
                    TAG,
                    "MonadoView: passthrough touch region unavailable; overlay stays fully touchable",
                    t);
        }
    }

    private MonadoView(Context context, long nativePointer) {
        this(context);

        nativeCounterpart = new NativeCounterpart(nativePointer);
    }

    /**
     * Construct and start attaching a MonadoView to a client application.
     *
     * @param activity The activity to attach to.
     * @return The MonadoView instance created and asynchronously attached.
     */
    @NonNull @Keep
    public static MonadoView attachToActivity(@NonNull final Activity activity) {
        return attachToActivity(activity, null);
    }

    /**
     * Construct and start attaching a MonadoView to a client application, observing the surface
     * lifecycle.
     *
     * <p>The listener is set before the view is posted to the window manager, so the first
     * surfaceCreated is never missed.
     *
     * @param activity The activity to attach to.
     * @param listener Surface lifecycle observer, may be null.
     * @return The MonadoView instance created and asynchronously attached.
     */
    @NonNull @Keep
    public static MonadoView attachToActivity(
            @NonNull final Activity activity, @Nullable SurfaceStateListener listener) {
        final MonadoView view = new MonadoView(activity);
        view.surfaceStateListener = listener;
        WindowManager.LayoutParams lp = new WindowManager.LayoutParams();
        lp.flags =
                WindowManager.LayoutParams.FLAG_FULLSCREEN
                        | WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE;
        attachToWindow(activity, view, lp);
        return view;
    }

    /**
     * Construct and start attaching a MonadoView to window.
     *
     * @param displayContext Display context used for looking for target window.
     * @param nativePointer The native android_custom_surface pointer, cast to a long.
     * @param lp Layout parameters associated with view.
     * @return The MonadoView instance created and asynchronously attached.
     */
    @NonNull @Keep
    public static MonadoView attachToWindow(
            @NonNull final Context displayContext,
            long nativePointer,
            WindowManager.LayoutParams lp)
            throws IllegalArgumentException {
        final MonadoView view = new MonadoView(displayContext, nativePointer);
        attachToWindow(displayContext, view, lp);
        return view;
    }

    private static void attachToWindow(
            @NonNull final Context context,
            @NonNull MonadoView view,
            @NonNull WindowManager.LayoutParams lp) {
        Handler handler = new Handler(Looper.getMainLooper());
        handler.post(
                () -> {
                    Log.d(TAG, "Start adding view to window");
                    WindowManager wm =
                            (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
                    wm.addView(view, lp);

                    SystemUiController systemUiController = new SystemUiController(view);
                    systemUiController.hide();
                });
    }

    /**
     * Remove given MonadoView from window.
     *
     * @param view The view to remove.
     */
    @Keep
    public static void removeFromWindow(@NonNull MonadoView view) {
        // #558: when we're already on the UI thread (e.g. the service's onDestroy →
        // MonadoImpl.shutdown → nativeDestroyServiceOverlay), use removeViewImmediate:
        // it detaches the view synchronously. Plain removeView() only *schedules*
        // the removal for the next looper traversal — which never runs when the
        // service's MainLooper is shutting down, so the overlay's last frame stays
        // frozen on the launcher. Off the UI thread, post a normal removeView.
        if (Looper.myLooper() == Looper.getMainLooper()) {
            Log.d(TAG, "Removing view from window (immediate)");
            WindowManager wm =
                    (WindowManager) view.getContext().getSystemService(Context.WINDOW_SERVICE);
            wm.removeViewImmediate(view);
        } else {
            new Handler(Looper.getMainLooper())
                    .post(
                            () -> {
                                Log.d(TAG, "Start removing view from window");
                                WindowManager wm =
                                        (WindowManager)
                                                view.getContext()
                                                        .getSystemService(Context.WINDOW_SERVICE);
                                wm.removeView(view);
                            });
        }
    }

    @NonNull @Keep
    public static DisplayMetrics getDisplayMetrics(@NonNull Context context) {
        DisplayMetrics displayMetrics = new DisplayMetrics();
        WindowManager wm = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
        wm.getDefaultDisplay().getRealMetrics(displayMetrics);
        return displayMetrics;
    }

    @Keep
    public static float getDisplayRefreshRate(@NonNull Context context) {
        WindowManager wm = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
        return wm.getDefaultDisplay().getRefreshRate();
    }

    /**
     * Get the width of the specified display mode on the specified display ID
     *
     * <p>If the specified mode ID is not in the list of supported mode IDs for the specified
     * display ID, then a value of 0 is returned.
     *
     * @param context Display context used for looking for target window.
     * @param display The display ID for which the mode is to be queried.
     * @param displayModeId The display mode ID for which the width is returned. This is a
     *     zero-indexed mode ID.
     * @return The width in pixels for the specified mode ID on the specified display.
     */
    @Keep
    public static int getDisplayModeIdWidth(
            @NonNull final Context context, int display, int displayModeId) {
        DisplayManager dm = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
        Display dp = dm.getDisplay(display);
        Display.Mode[] modes = dp.getSupportedModes();
        if (modes.length > displayModeId) {
            return modes[displayModeId].getPhysicalWidth();
        }
        return 0;
    }

    /**
     * Get the height of the specified display mode on the specified display ID
     *
     * <p>If the specified mode ID is not in the list of supported mode IDs for the specified
     * display ID, then a value of 0 is returned.
     *
     * @param context Display context used for looking for target window.
     * @param display The display ID for which the mode is to be queried.
     * @param displayModeId The display mode ID for which the height is returned. This is a
     *     zero-indexed mode ID.
     * @return The height in pixels for the specified mode ID on the specified display.
     */
    @Keep
    public static int getDisplayModeIdHeight(
            @NonNull final Context context, int display, int displayModeId) {
        DisplayManager dm = (DisplayManager) context.getSystemService(Context.DISPLAY_SERVICE);
        Display dp = dm.getDisplay(display);
        Display.Mode[] modes = dp.getSupportedModes();
        if (modes.length > displayModeId) {
            return modes[displayModeId].getPhysicalHeight();
        }
        return 0;
    }

    @Keep
    public static float[] getSupportedRefreshRates(@NonNull Context context) {
        WindowManager wm = (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
        TreeSet<Float> rateSet = new TreeSet<>();
        Display.Mode[] modes = wm.getDefaultDisplay().getSupportedModes();
        for (Display.Mode mode : modes) {
            rateSet.add(mode.getRefreshRate());
        }
        float[] rates = new float[rateSet.size()];
        int i = 0;
        for (Float f : rateSet) {
            rates[i++] = f;
        }
        return rates;
    }

    @Keep
    public long getNativePointer() {
        if (nativeCounterpart == null) {
            return 0;
        }
        return nativeCounterpart.getNativePointer();
    }

    /**
     * Block up to a specified amount of time, waiting for the surfaceCreated callback to be fired
     * and populate the currentSurfaceHolder.
     *
     * <p>If it returns a SurfaceHolder, the `usedByNativeCode` flag will be set.
     *
     * <p>Called by native code!
     *
     * @param wait_ms Max duration you prefer to wait, in milliseconds. Spurious wakeups mean this
     *     not be totally precise.
     * @return A SurfaceHolder or null.
     */
    @Keep
    public @Nullable SurfaceHolder waitGetSurfaceHolder(int wait_ms) {
        long currentTime = SystemClock.uptimeMillis();
        long timeout = currentTime + wait_ms;
        SurfaceHolder ret = null;
        synchronized (currentSurfaceHolderSync) {
            ret = currentSurfaceHolder;
            while (currentSurfaceHolder == null && SystemClock.uptimeMillis() < timeout) {
                try {
                    currentSurfaceHolderSync.wait(wait_ms, 0);
                    ret = currentSurfaceHolder;
                } catch (InterruptedException e) {
                    // stop waiting
                    break;
                }
            }
        }
        if (ret != null) {
            if (nativeCounterpart != null) nativeCounterpart.markAsUsedByNativeCode();
        }
        return ret;
    }

    /**
     * Change the flag and notify those waiting on it, to indicate that native code is done with
     * this object.
     *
     * <p>Called by native code!
     */
    @Keep
    public void markAsDiscardedByNative() {
        if (nativeCounterpart != null) nativeCounterpart.markAsDiscardedByNative(TAG);
    }

    @Override
    public void surfaceCreated(@NonNull SurfaceHolder surfaceHolder) {
        synchronized (currentSurfaceHolderSync) {
            currentSurfaceHolder = surfaceHolder;
            currentSurfaceHolderSync.notifyAll();
        }
        Log.i(TAG, "surfaceCreated: Got a surface holder!");
        if (surfaceStateListener != null) {
            surfaceStateListener.onSurfaceAvailable(surfaceHolder);
        }
    }

    @Override
    public void surfaceChanged(
            @NonNull SurfaceHolder surfaceHolder, int format, int width, int height) {

        synchronized (currentSurfaceHolderSync) {
            currentSurfaceHolder = surfaceHolder;
            this.format = format;
            this.width = width;
            this.height = height;
            currentSurfaceHolderSync.notifyAll();
        }
        Log.i(
                TAG,
                "surfaceChanged, w = "
                        + this.width
                        + " h = "
                        + this.height
                        + " holder "
                        + currentSurfaceHolder.toString());
        // The native side pulls this updated surface via android_custom_surface_
        // refresh_window() on its next poll (#507) — no push needed here. The
        // out-of-process client instead observes via the listener (#528).
        if (surfaceStateListener != null) {
            surfaceStateListener.onSurfaceAvailable(surfaceHolder);
        }
    }

    @Override
    public void surfaceDestroyed(@NonNull SurfaceHolder surfaceHolder) {
        Log.i(TAG, "surfaceDestroyed: Lost our surface.");
        boolean lost = false;
        synchronized (currentSurfaceHolderSync) {
            if (surfaceHolder == currentSurfaceHolder) {
                currentSurfaceHolder = null;
                lost = true;
            }
        }
        if (lost) {
            // The native side notices the surface is gone on its next pull
            // (android_custom_surface_refresh_window → waitGetSurfaceHolder returns
            // null → the compositor tears its VkSurfaceKHR down). #507
            //
            // We deliberately do NOT block on nativeCounterpart.blockUntilNativeDiscard
            // here: that wait only completes when native DESTROYS the counterpart
            // (session teardown), so on a plain background→card cycle it would block
            // this UI thread forever — and a frozen UI thread can never deliver the
            // surfaceCreated/surfaceChanged that resume needs. The teardown handshake
            // stays in the session-destroy path (android_custom_surface destructor →
            // markAsDiscardedByNative); it does not belong on a transient surface loss.
            if (surfaceStateListener != null) {
                surfaceStateListener.onSurfaceDestroyed();
            }
        }
    }

    @Override
    public void surfaceRedrawNeeded(@NonNull SurfaceHolder surfaceHolder) {
        //        currentSurfaceHolder = surfaceHolder;
        Log.i(TAG, "surfaceRedrawNeeded");
    }
}
