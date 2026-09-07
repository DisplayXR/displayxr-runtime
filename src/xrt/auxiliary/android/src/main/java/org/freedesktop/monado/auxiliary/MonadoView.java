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
import android.app.Application;
import android.content.Context;
import android.os.Bundle;
import android.graphics.PixelFormat;
import android.graphics.Point;
import android.graphics.Rect;
import android.graphics.Region;
import android.hardware.display.DisplayManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Choreographer;
import android.view.Display;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.ViewTreeObserver;
import android.view.WindowManager;
import android.widget.FrameLayout;
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

        /**
         * The view's on-screen rectangle changed (ADR-036 D6, #1033). UI thread, fired from a
         * Choreographer callback, only when the value actually changed.
         *
         * <p>A pure window MOVE on Android raises no resize and no public callback — SurfaceFlinger
         * just repositions the layer with the old buffer — so the compositor cannot learn where the
         * window went from the surface. Polling {@link android.view.View#getLocationOnScreen} once
         * per frame is the only signal there is.
         *
         * @param x window left edge in physical screen pixels (may be negative)
         * @param y window top edge in physical screen pixels
         * @param w view width in physical screen pixels
         * @param h view height in physical screen pixels
         * @param displayId {@code Display.getDisplayId()} the rect is expressed in
         * @param dispW display width in physical screen pixels, CURRENT rotation (#1034)
         * @param dispH display height in physical screen pixels, CURRENT rotation (#1034)
         */
        void onWindowRectChanged(int x, int y, int w, int h, int displayId, int dispW, int dispH);
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

    /** Guards the window-attach state below (touched from native threads and the UI thread). */
    private final Object attachSync = new Object();

    /**
     * #1358: the MonadoView is never handed to the WindowManager directly — it is the single child
     * of this plain {@link FrameLayout}, and the FrameLayout is what {@code wm.addView} gets.
     *
     * <p>A View added straight to the WindowManager has the ViewRootImpl as its {@code mParent},
     * and the ViewRootImpl NULLS that parent as part of tearing the window down
     * ({@code dispatchDetachedFromWindow} → {@code assignParent(null)}). A {@link SurfaceView}
     * dereferences {@code mParent} unconditionally in two places —
     * {@code SurfaceView.onAttachedToWindow} (line 294 on Android 13) and
     * {@code SurfaceView.performDrawFinished} (line 396) — both doing
     * {@code mParent.requestTransparentRegion(this)}. When a still-pending attach callback or a
     * still-pending draw-finished callback lands after the window went away, that is a
     * {@code NullPointerException} on the UI thread, i.e. the app process dies. That is exactly
     * what happens when a hosted app's Activity is relaunched (a config change it does not
     * declare, a locale change, the lockscreen bring-up of #1358) — the old view's callback fires
     * while the new Activity is already coming up.
     *
     * <p>With a FrameLayout in between, the SurfaceView's {@code mParent} is the FrameLayout for
     * its whole life — we never call {@code removeView} on it — and
     * {@code ViewGroup.requestTransparentRegion} is itself null-safe on its own parent. This is
     * the ordinary shape every Android app uses (a SurfaceView inside a ViewGroup), which is why
     * no ordinary app ever hits this. Everything else is unchanged: the FrameLayout has no
     * background (draws nothing, so translucency and {@code setZOrderMediaOverlay} behave the
     * same), it dispatches touch straight down to its only child (#499 forwarding), and
     * {@code getLocationOnScreen} on the child still returns absolute screen coordinates (#1367).
     */
    @GuardedBy("attachSync")
    @Nullable
    private ViewGroup windowContainer = null;

    /** True once {@code wm.addView} actually ran for {@link #windowContainer}. */
    @GuardedBy("attachSync")
    private boolean addedToWindow = false;

    /**
     * Set when the native side gives up before the posted {@code wm.addView} ever ran, so the add
     * is dropped instead of racing a remove posted behind it (#1358 bug 2).
     */
    @GuardedBy("attachSync")
    private boolean attachCancelled = false;

    /**
     * #1389: the hook that takes the hosted window down when the host Activity is destroyed.
     *
     * <p>Nothing used to remove it. The runtime keeps its {@code android_custom_surface} alive
     * across xrEndSession/xrBeginSession on purpose (#507) and never retired it, so on an Activity
     * RELAUNCH the framework found the window still attached to the dying Activity's token, logged
     * {@code WindowLeaked}, and swept it with a POSTED {@code ViewRootImpl.die()} — i.e. the
     * ANativeWindow was still alive, and still published to native as valid, while the relaunched
     * Activity was already creating its session. {@code vkCreateAndroidSurfaceKHR} on that dead
     * window fails {@code VK_ERROR_NATIVE_WINDOW_IN_USE_KHR} (the Android loader maps every
     * {@code native_window_api_connect} failure onto that code), and the session never comes up.
     *
     * <p>This is deliberately a JAVA hook rather than the runtime's own native lifecycle callbacks
     * ({@code android_lifecycle_callbacks.cpp}): for a {@code native_app_glue} app the runtime
     * instance is already destroyed by then. {@code NativeActivity.onDestroy} runs
     * {@code unloadNativeCode()} — which BLOCKS the UI thread until {@code android_main} has
     * returned, i.e. past {@code xrDestroyInstance} and the listener's own unregistration — and
     * only then calls {@code super.onDestroy()}, which is what dispatches
     * {@code onActivityDestroyed}. That same ordering is why this can be synchronous: the native
     * side is provably gone, nothing is presenting to the surface any more, and we are on the UI
     * thread with the framework's leak sweep still ahead of us, so {@code removeViewImmediate}
     * detaches the window before {@code WindowManagerGlobal.closeAll} ever sees it.
     */
    @Nullable private Application.ActivityLifecycleCallbacks hostDestroyHook = null;

    public MonadoView(Context context) {
        super(context);

        if (context instanceof Activity) {
            Activity activity = (Activity) context;
            hostActivity = activity;
            systemUiController = new SystemUiController(activity.getWindow().getDecorView());
            systemUiController.hide();
            pinDisplayModeIfRequested(activity);
            registerHostDestroyHook(activity);
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
            // #1367 S9 fallback (c): the OEM freeform leash is inverted on the way IN,
            // so raw-vs-local on a REAL drag measures the scale. Free — we already see
            // every event here.
            measureTouchScale(event);
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
        startWindowRectPoll();
    }

    @Override
    protected void onDetachedFromWindow() {
        stopWindowRectPoll();
        super.onDetachedFromWindow();
    }

    // ---------------------------------------------------------------- window rect (#1033)

    private final int[] locationOnScreen = new int[2];
    private int lastRectX = Integer.MIN_VALUE;
    private int lastRectY = Integer.MIN_VALUE;
    private int lastRectW = -1;
    private int lastRectH = -1;
    private int lastRectDisplayId = -1;
    private int lastRectDispW = -1;
    private int lastRectDispH = -1;
    private boolean windowRectPollRunning = false;
    @Nullable private Choreographer.FrameCallback windowRectCallback = null;

    // ---- mini-window 1:1 weave (#1367 S9 / #1277). All touched on the UI thread only.
    private static final float TOUCH_MIN_SPAN_PX = 40.0f;
    private static final long VENDOR_RETRY_MS = 500;
    private final int[] oneToOneSize = new int[2];
    private boolean miniOneToOneApplied = false;
    private int miniBufW = 0;
    private int miniBufH = 0;
    private float miniVendorScale = 0f;
    private long miniVendorLastTryMs = 0;
    private boolean miniCrossChecked = false;
    private boolean miniScaleMismatchLogged = false;
    @Nullable private String miniScaleSource = null;
    private int miniPropCached = -1;
    private float touchDownRawX = 0f;
    private float touchDownRawY = 0f;
    private float touchDownX = 0f;
    private float touchDownY = 0f;
    private boolean touchDownValid = false;
    private float touchScale = 0f;
    private float touchScaleSpan = 0f;

    /**
     * Sample this view's on-screen rect once per frame and report changes (ADR-036 D6, #1033).
     *
     * <p>Choreographer rather than a layout/position listener because a pure window MOVE produces
     * neither: {@code WindowFrames.didFrameSizeChange} compares w/h only, so the move goes out as a
     * {@code oneway IWindow.moved} that updates {@code mAttachInfo.mWindowLeft/Top} and nothing
     * else — no layout, no invalidate, no callback. SurfaceView derives {@code positionChanged}
     * from {@code getLocationInWindow()}, which is unchanged by a move. Meanwhile SurfaceFlinger
     * has already repositioned the layer with the OLD buffer, so an un-updated weave keeps a stale
     * interlace phase for the whole drag.
     *
     * <p>Cost is one {@code getLocationOnScreen} per frame plus five int compares; the listener
     * (and therefore the binder hop) only fires on an actual change.
     *
     * <p>TRAP: an OEM that applies {@code OVERRIDE_SANDBOX_VIEW_BOUNDS_APIS} makes
     * {@code getLocationOnScreen} return WINDOW-relative coordinates, which would silently report
     * (0,0) for every window. Clients opt out with
     * {@code <property android:name="android.window.PROPERTY_COMPAT_ALLOW_SANDBOXING_VIEW_BOUNDS_APIS"
     * android:value="false"/>} in their own manifest — see docs/guides/displayxr-app-rules.md.
     */
    private void startWindowRectPoll() {
        if (windowRectPollRunning) {
            return;
        }
        if (surfaceStateListener == null && !reportsRectToNative()) {
            // Nobody to report to — e.g. the SERVICE-owned overlay (#558) or the weave
            // satellite's surface, which have no host Activity and no IPC client to
            // forward to. Don't burn a Choreographer callback per frame for nothing.
            // The listener is assigned in attachToActivity() before the view reaches
            // the window manager, so a real client is always registered by the time
            // onAttachedToWindow fires; the in-process hosted view (#1367) reports
            // straight to native instead — see reportsRectToNative().
            return;
        }
        windowRectPollRunning = true;
        windowRectCallback =
                new Choreographer.FrameCallback() {
                    @Override
                    public void doFrame(long frameTimeNanos) {
                        if (!windowRectPollRunning) {
                            return;
                        }
                        sampleWindowRect();
                        Choreographer.getInstance().postFrameCallback(this);
                    }
                };
        Choreographer.getInstance().postFrameCallback(windowRectCallback);
    }

    private void stopWindowRectPoll() {
        windowRectPollRunning = false;
        if (windowRectCallback != null) {
            Choreographer.getInstance().removeFrameCallback(windowRectCallback);
            windowRectCallback = null;
        }
    }

    private void sampleWindowRect() {
        int w = getWidth();
        int h = getHeight();
        if (w <= 0 || h <= 0) {
            return; // not laid out yet
        }
        getLocationOnScreen(locationOnScreen);
        int x = locationOnScreen[0];
        int y = locationOnScreen[1];
        int displayId = 0;
        int dispW = 0;
        int dispH = 0;
        Display display = getDisplay();
        if (display != null) {
            displayId = display.getDisplayId();
            // #1034: the panel extent in the CURRENT rotation, which is the frame the
            // rect above is expressed in. The per-window Kooima needs it to place the
            // window centre relative to the panel centre, and it cannot be derived from
            // the runtime's display info: that is the NATURAL-orientation panel (this
            // device is natively portrait 1600x2560 and runs landscape 2560x1600), and a
            // sub-panel window fits inside BOTH orderings, so "which way is the panel
            // held" is genuinely ambiguous without asking Android. getRealSize is
            // deprecated but is the only call that gives the raw panel extent (not the
            // app bounds) on every API level we ship to.
            Point real = new Point();
            display.getRealSize(real);
            dispW = real.x;
            dispH = real.y;
        }
        // #1277/#1367 S9: in an OEM-scaled container the numbers above are the
        // window's LOGICAL size; the on-screen extent is scale x that. Re-size the
        // surface's buffer to the physical extent and publish THAT, so every
        // downstream consumer (view dims, Kooima, DP origin) works in panel pixels
        // and SurfaceFlinger's composed transform collapses to identity. Returns
        // false — publish the logical rect, keep the 2D fallback — whenever the
        // window fits the panel, the scale is not known yet, or the surface has not
        // yet come back at the new size.
        if (updateMiniWindowOneToOne(x, y, w, h, dispW, dispH, oneToOneSize)) {
            w = oneToOneSize[0];
            h = oneToOneSize[1];
        }

        if (x == lastRectX
                && y == lastRectY
                && w == lastRectW
                && h == lastRectH
                && displayId == lastRectDisplayId
                && dispW == lastRectDispW
                && dispH == lastRectDispH) {
            return;
        }
        lastRectX = x;
        lastRectY = y;
        lastRectW = w;
        lastRectH = h;
        lastRectDisplayId = displayId;
        lastRectDispW = dispW;
        lastRectDispH = dispH;
        Log.i(
                TAG,
                "windowRect: "
                        + x
                        + ","
                        + y
                        + " "
                        + w
                        + "x"
                        + h
                        + " display "
                        + displayId
                        + " panel "
                        + dispW
                        + "x"
                        + dispH);
        SurfaceStateListener listener = surfaceStateListener;
        if (listener != null) {
            listener.onWindowRectChanged(x, y, w, h, displayId, dispW, dispH);
        } else if (reportsRectToNative()) {
            try {
                nativeWindowRectChanged(
                        nativeCounterpart.getNativePointer(), x, y, w, h, displayId, dispW, dispH);
            } catch (UnsatisfiedLinkError e) {
                // The native side that owns this view did not register the entry point
                // (an older runtime .so, or a process that never called
                // android_custom_surface_async_start). Nothing to report to; stop
                // polling rather than throw once per frame.
                Log.w(TAG, "nativeWindowRectChanged not registered; window rect stays unpublished", e);
                stopWindowRectPoll();
            }
        }
    }

    /**
     * #1367: the in-process {@code _hosted} fallback. The runtime spawns this view on the app's
     * Activity (native-owned, {@code hostActivity != null}) and has no Java listener, so until
     * now its window rect was never published: a freeform-resized hosted window weaved with a
     * display-sized canvas squeezed into the surface and a display-anchored phase. Report the rect
     * straight to native, which feeds the same {@code android_globals} sink an app's own
     * {@code xrSetAndroidWindowGeometryDXR} uses. The service-owned overlay and the weave
     * satellite have no host Activity and are excluded: in the service process that sink carries
     * the CLIENT's rect and must not be clobbered by the overlay's own.
     */
    private boolean reportsRectToNative() {
        return hostActivity != null && nativeCounterpart != null && nativeCounterpart.getNativePointer() != 0;
    }

    // ------------------------------------------------- mini-window 1:1 weave (#1367 S9 / #1277)

    /**
     * The OEM's "window reply" mini-window scales the whole task with a SurfaceFlinger leash
     * (measured on NP02J: {@code SCALE TRANSLATE 0.67 @ (1757,236)}), so a 1080x1685 logical
     * window lands on the panel as 724x1129 physical pixels. The vendor interlacer is strict
     * 1:1 buffer→panel: any resample between the woven buffer and the panel destroys the
     * interlace, which is why {@code vk_android_update_container_scaled} degrades such a window
     * to flat 2D.
     *
     * <p>The fix is to make SF's COMPOSED transform identity rather than to fight it: hand the
     * surface a buffer of {@code round(logical * scale)} pixels, and SF's buffer→layer scale
     * ({@code 1080/724}) times the leash ({@code 0.67}) multiplies out to 1.0. The rect we then
     * publish is the physical one, so the compositor's view dims, the per-window Kooima and the
     * DP's screen origin are all in panel pixels — the same frame the weave happens in.
     *
     * <p>TRAP: size the buffer to {@code round(scale * logical)} — which is what the vendor API's
     * Rect and the layer's {@code coveredRegion} both report — and NOT to SurfaceFlinger's
     * {@code displayFrame} (732x1137 here). displayFrame includes the task layer's shadow
     * ({@code shadowRadius} 6 x 0.67 ~ 4 px a side); sizing to it re-introduces an 8 px resample,
     * i.e. exactly the thing this exists to remove.
     *
     * @param outSize receives the physical width/height when this returns true
     * @return true when the 1:1 buffer is in effect AND the surface has already come back at that
     *     size, i.e. the physical rect is the one to publish
     */
    private boolean updateMiniWindowOneToOne(
            int x, int y, int w, int h, int dispW, int dispH, int[] outSize) {
        // Same predicate as the compositor's CONTAINER_SCALED tell, deliberately: one
        // definition of "this window is being scaled by the container".
        boolean tell =
                dispW > 0 && dispH > 0 && (x < 0 || y < 0 || x + w > dispW || y + h > dispH);

        if (!tell || !isMiniWindow1to1Enabled()) {
            if (miniOneToOneApplied) {
                getHolder().setSizeFromLayout();
                miniOneToOneApplied = false;
                miniBufW = 0;
                miniBufH = 0;
                miniVendorScale = 0f;
                Log.i(
                        TAG,
                        "miniWindow1to1: OFF ("
                                + (tell ? "disabled by debug.dxr.miniwindow_1to1" : "window fits the panel")
                                + ") — surface back to layout size");
            }
            return false;
        }

        float s = resolveMiniScale(w, h, dispW, dispH);
        if (s <= 0f) {
            // Not known yet (no vendor API and no real touch measured), or the two
            // sources disagreed. Publishing the logical rect keeps the honest 2D
            // fallback, which is the direction to fail in.
            return false;
        }

        int bw = Math.round(w * s);
        int bh = Math.round(h * s);
        if (bw <= 0 || bh <= 0) {
            return false;
        }
        if (!miniOneToOneApplied || bw != miniBufW || bh != miniBufH) {
            miniBufW = bw;
            miniBufH = bh;
            miniOneToOneApplied = true;
            getHolder().setFixedSize(bw, bh);
            // ONE line per transition — a lifecycle event, never a per-frame one.
            Log.i(
                    TAG,
                    "miniWindow1to1: ON scale="
                            + s
                            + " source="
                            + miniScaleSource
                            + " logical "
                            + w
                            + "x"
                            + h
                            + " -> buffer "
                            + bw
                            + "x"
                            + bh
                            + " (SF buffer->layer x leash should compose to identity)");
        }

        // Ordering: setFixedSize is a request. Until the surface actually comes back at
        // that size (surfaceChanged, which re-samples), publishing the physical rect
        // would have the compositor weave at a size the buffer does not have — one
        // visibly wrong frame. Publish the logical rect until then.
        synchronized (currentSurfaceHolderSync) {
            if (this.width != bw || this.height != bh) {
                return false;
            }
        }
        outSize[0] = bw;
        outSize[1] = bh;
        return true;
    }

    /**
     * The container scale, preferring the vendor API and falling back to the touch ratio. When
     * both are available they are cross-checked once: on this firmware they agree exactly
     * (0.670000), so a disagreement means accessibility magnification is multiplied in, or the
     * firmware changed — neither is a number to guess at, so log once and stay on the 2D
     * fallback.
     */
    private float resolveMiniScale(int w, int h, int dispW, int dispH) {
        float api = queryVendorWrScale(w, h, dispW, dispH);
        float touch = touchScale;

        if (api > 0f && touch > 0f) {
            if (Math.abs(api - touch) > 0.01f) {
                if (!miniScaleMismatchLogged) {
                    miniScaleMismatchLogged = true;
                    Log.w(
                            TAG,
                            "miniWindow1to1: vendor-api scale "
                                    + api
                                    + " disagrees with the measured touch ratio "
                                    + touch
                                    + " — accessibility magnification, or a firmware change. "
                                    + "Staying on the 2D fallback rather than picking one.");
                }
                return 0f;
            }
            if (!miniCrossChecked) {
                miniCrossChecked = true;
                Log.i(TAG, "miniWindow1to1: cross-check OK, vendor-api " + api + " == touch " + touch);
            }
        }

        if (api > 0f) {
            miniScaleSource = "vendor-api";
            return api;
        }
        if (touch > 0f) {
            miniScaleSource = "touch";
            return touch;
        }
        return 0f;
    }

    /**
     * S9 probe result (b): {@code ActivityManager.getDefaultWindowParamByTaskForNormalWr(taskId)}
     * is a hidden TEST-API that is NOT on this build's blocklist and returns the POST-SCALE
     * on-screen Rect for an ordinary app uid, no permission needed.
     *
     * <p>GATED ON THE TELL by the sole caller, and that gate is load-bearing: the method returns
     * the NOMINAL window-reply placement whether or not the app is actually in a mini-window — in
     * fullscreen it still answers {@code Rect(1757,236-2481,1365)}. An ungated read would shrink a
     * fullscreen buffer to 724x1129.
     *
     * @return the scale, or 0 when unavailable or implausible
     */
    private float queryVendorWrScale(int w, int h, int dispW, int dispH) {
        if (miniVendorScale > 0f) {
            return miniVendorScale; // resolved once per scaled-container episode
        }
        long now = SystemClock.uptimeMillis();
        if (now - miniVendorLastTryMs < VENDOR_RETRY_MS) {
            return 0f; // don't re-reflect every frame while it keeps failing
        }
        miniVendorLastTryMs = now;

        Activity activity = hostActivity;
        if (activity == null) {
            return 0f;
        }
        try {
            Object am = activity.getSystemService(Context.ACTIVITY_SERVICE);
            if (am == null) {
                return 0f;
            }
            Rect r = null;
            try {
                r =
                        (Rect)
                                am.getClass()
                                        .getMethod("getDefaultWindowParamByTaskForNormalWr", int.class)
                                        .invoke(am, activity.getTaskId());
            } catch (Throwable ignored) {
                r = null;
            }
            if (r == null || r.width() <= 0 || r.height() <= 0) {
                try {
                    r =
                            (Rect)
                                    am.getClass()
                                            .getMethod("getDefaultWindowParamForNormalWr", boolean.class)
                                            .invoke(am, Boolean.FALSE);
                } catch (Throwable ignored) {
                    return 0f;
                }
            }
            if (r == null || r.width() <= 0 || r.height() <= 0) {
                return 0f;
            }
            // A result that does not fit the panel is not an on-screen rect.
            if (r.width() > dispW || r.height() > dispH) {
                return 0f;
            }
            float sx = r.width() / (float) w;
            float sy = r.height() / (float) h;
            if (sx < 0.2f || sx > 1.0f || Math.abs(sx - sy) > 0.02f) {
                return 0f;
            }
            miniVendorScale = (sx + sy) * 0.5f;
            return miniVendorScale;
        } catch (Throwable t) {
            return 0f;
        }
    }

    /**
     * S9 probe result (c): the platform inverts the leash on the way in, so on a REAL dispatched
     * drag {@code |Δraw| / |Δlocal|} is the container scale (measured 0.670000, max residual 1e-4
     * px over 100 samples), and it needs neither a vendor API nor a permission. Differences, so
     * the view position and the insets cancel. Synthetic events ({@code MotionEvent.obtain}) read
     * 1.0 and are rejected by the plausibility band below, as is a fullscreen window.
     */
    private void measureTouchScale(android.view.MotionEvent ev) {
        final int action = ev.getActionMasked();
        if (action == android.view.MotionEvent.ACTION_DOWN) {
            touchDownRawX = ev.getRawX();
            touchDownRawY = ev.getRawY();
            touchDownX = ev.getX();
            touchDownY = ev.getY();
            touchDownValid = true;
            return;
        }
        if (!touchDownValid
                || (action != android.view.MotionEvent.ACTION_MOVE
                        && action != android.view.MotionEvent.ACTION_UP)) {
            return;
        }
        if (action == android.view.MotionEvent.ACTION_UP) {
            touchDownValid = false;
        }
        float localSpan = Math.abs(ev.getX() - touchDownX) + Math.abs(ev.getY() - touchDownY);
        if (localSpan < TOUCH_MIN_SPAN_PX) {
            return; // too short to divide by
        }
        float rawSpan = Math.abs(ev.getRawX() - touchDownRawX) + Math.abs(ev.getRawY() - touchDownRawY);
        float s = rawSpan / localSpan;
        if (s < 0.2f || s > 0.999f) {
            return; // fullscreen (1.0), a synthetic event, or nonsense
        }
        if (localSpan > touchScaleSpan) {
            touchScaleSpan = localSpan;
            touchScale = s;
        }
    }

    /**
     * {@code debug.dxr.miniwindow_1to1} — default ON. Set to 0 to A/B against the old behaviour
     * (the honest 2D fallback in {@code vk_android_update_container_scaled}).
     *
     * <p>READ ONCE PER PROCESS and cached, the same contract as {@code debug.dxr.weave_satellite}:
     * changing it mid-run needs an app restart. That is not laziness — flipping it live was
     * measured to WEDGE the app. It re-sizes the surface underneath a weave that is already in
     * flight, and the vendor's `leia_cnsdk_weave` then sits forever in `vkWaitForFences` inside
     * `libleiaCore-impl.so` while the app thread blocks on the compositor mutex in
     * `vk_compositor_layer_commit` (ANR trace, NP02J, 2026-09-07). The buffer size may only change
     * where nothing is mid-weave — which on this OEM is the container transition itself, since
     * entering and leaving the mini-window destroys and recreates the surface.
     */
    private boolean isMiniWindow1to1Enabled() {
        if (miniPropCached >= 0) {
            return miniPropCached == 1;
        }
        int on = 1;
        try {
            Class<?> sp = Class.forName("android.os.SystemProperties");
            String v = (String) sp.getMethod("get", String.class).invoke(null, "debug.dxr.miniwindow_1to1");
            if (v != null && !v.isEmpty()) {
                on =
                        (v.startsWith("0") || v.startsWith("f") || v.startsWith("F") || v.startsWith("n")
                                        || v.startsWith("N"))
                                ? 0
                                : 1;
            }
        } catch (Exception e) {
            on = 1;
        }
        miniPropCached = on;
        return on == 1;
    }

    /**
     * Registered by native code ({@code android_custom_surface.cpp}, {@code RegisterNatives}) —
     * the runtime .so is dlopen'ed by the OpenXR loader, so a {@code Java_…} symbol lookup would
     * never find it. Arguments as {@link SurfaceStateListener#onWindowRectChanged}.
     */
    private static native void nativeWindowRectChanged(
            long nativePointer, int x, int y, int w, int h, int displayId, int dispW, int dispH);

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
            @NonNull final MonadoView view,
            @NonNull WindowManager.LayoutParams lp) {
        // #1358: hand the WindowManager a FrameLayout that owns the MonadoView, never the
        // SurfaceView itself — see the windowContainer field comment for why.
        final FrameLayout container = new FrameLayout(context);
        container.addView(
                view,
                new FrameLayout.LayoutParams(
                        FrameLayout.LayoutParams.MATCH_PARENT,
                        FrameLayout.LayoutParams.MATCH_PARENT));
        synchronized (view.attachSync) {
            view.windowContainer = container;
        }

        Handler handler = new Handler(Looper.getMainLooper());
        handler.post(
                () -> {
                    synchronized (view.attachSync) {
                        if (view.attachCancelled) {
                            // Native gave up (timeout / session teardown) before this posted
                            // add ever ran. Adding now would only be followed by a remove
                            // racing the very traversal that attaches it (#1358 bug 2).
                            Log.d(TAG, "Discarding pending add: native already gave up");
                            return;
                        }
                        Log.d(TAG, "Start adding view to window");
                        WindowManager wm =
                                (WindowManager) context.getSystemService(Context.WINDOW_SERVICE);
                        wm.addView(container, lp);
                        view.addedToWindow = true;
                    }

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
        view.unregisterHostDestroyHook();
        view.detachFromWindow();
    }

    /**
     * Take the container back out of the WindowManager. Idempotent, and safe to call from either
     * the native side ({@link #removeFromWindow}) or the host Activity's destroy (#1389) — whoever
     * gets there first does the work and the other becomes a no-op.
     */
    private void detachFromWindow() {
        // #1358: what the WindowManager holds is the FrameLayout container, so that is what has
        // to come back out. The MonadoView is deliberately LEFT inside the container — keeping
        // its ViewParent alive is the whole point of the wrapper.
        final View target;
        synchronized (attachSync) {
            if (!addedToWindow) {
                // The posted wm.addView has not run yet (or never will). Cancel it instead of
                // posting a removeView behind it: that ordering is what made the pending
                // attach/draw-finished callbacks fire against a torn-down window (#1358).
                attachCancelled = true;
                Log.d(TAG, "detachFromWindow: cancelled a still-pending add");
                return;
            }
            attachCancelled = true;
            addedToWindow = false;
            target = windowContainer != null ? windowContainer : this;
        }

        final Context ctx = getContext();
        // #558: when we're already on the UI thread (e.g. the service's onDestroy →
        // MonadoImpl.shutdown → nativeDestroyServiceOverlay, or the host Activity's
        // onActivityDestroyed of #1389), use removeViewImmediate: it detaches the view
        // synchronously. Plain removeView() only *schedules* the removal for the next
        // looper traversal — which never runs when the service's MainLooper is shutting
        // down, so the overlay's last frame stays frozen on the launcher; and on an
        // Activity relaunch it lands AFTER the framework's own leak sweep, which is
        // exactly the race #1389 is about. Off the UI thread, post a normal removeView.
        if (Looper.myLooper() == Looper.getMainLooper()) {
            Log.d(TAG, "Removing view from window (immediate)");
            removeQuietly(ctx, target, true);
        } else {
            new Handler(Looper.getMainLooper())
                    .post(
                            () -> {
                                Log.d(TAG, "Start removing view from window");
                                removeQuietly(ctx, target, false);
                            });
        }
    }

    /**
     * WindowManager.removeView* throws IllegalArgumentException when the view is not (or no longer)
     * attached. That is a legitimate race now that two paths can remove the container — and it
     * would otherwise be an uncaught exception on the UI thread, i.e. process death.
     */
    private static void removeQuietly(
            @NonNull Context ctx, @NonNull View target, boolean immediate) {
        try {
            WindowManager wm = (WindowManager) ctx.getSystemService(Context.WINDOW_SERVICE);
            if (immediate) {
                wm.removeViewImmediate(target);
            } else {
                wm.removeView(target);
            }
        } catch (Throwable t) {
            Log.w(TAG, "Removing the hosted window failed (already gone?)", t);
        }
    }

    /** See {@link #hostDestroyHook}. */
    private void registerHostDestroyHook(@NonNull final Activity activity) {
        final Application app = activity.getApplication();
        if (app == null) {
            Log.w(TAG, "No Application on the host Activity — hosted window teardown hook skipped");
            return;
        }
        hostDestroyHook =
                new Application.ActivityLifecycleCallbacks() {
                    @Override
                    public void onActivityCreated(
                            @NonNull Activity a, @Nullable Bundle savedInstanceState) {}

                    @Override
                    public void onActivityStarted(@NonNull Activity a) {}

                    @Override
                    public void onActivityResumed(@NonNull Activity a) {}

                    @Override
                    public void onActivityPaused(@NonNull Activity a) {}

                    @Override
                    public void onActivityStopped(@NonNull Activity a) {}

                    @Override
                    public void onActivitySaveInstanceState(
                            @NonNull Activity a, @NonNull Bundle outState) {}

                    @Override
                    public void onActivityDestroyed(@NonNull Activity a) {
                        if (a != activity) {
                            return;
                        }
                        Log.i(TAG, "Host activity destroyed — removing the hosted window (#1389)");
                        unregisterHostDestroyHook();
                        detachFromWindow();
                    }
                };
        app.registerActivityLifecycleCallbacks(hostDestroyHook);
    }

    private void unregisterHostDestroyHook() {
        final Application.ActivityLifecycleCallbacks hook = hostDestroyHook;
        final Activity activity = hostActivity;
        hostDestroyHook = null;
        if (hook == null || activity == null) {
            return;
        }
        try {
            Application app = activity.getApplication();
            if (app != null) {
                app.unregisterActivityLifecycleCallbacks(hook);
            }
        } catch (Throwable t) {
            Log.w(TAG, "Could not unregister the hosted-window teardown hook", t);
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
        // #1367 S9: the mini-window 1:1 path publishes the PHYSICAL rect only once the
        // surface has actually come back at the requested buffer size. This is that
        // moment — re-sample now rather than waiting for the next Choreographer tick, so
        // the rect and the buffer change over together.
        if (windowRectPollRunning) {
            sampleWindowRect();
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

    /**
     * Ask the platform to hold ONE display mode, so the weave's schedule is not
     * re-based mid-session.
     *
     * <p>The panel here is variable-refresh and the platform re-rates it on
     * interaction — policy tuned for scrolling, where more updates are strictly
     * better. A 3D weave wants the opposite: the eye grades cadence stability,
     * and a rate switch is a discontinuity in the schedule the weave is
     * phase-locked to.
     *
     * <p>This is the Java-side mechanism, and it is the CORRECT one — it selects
     * an actual display mode rather than declaring a content rate the way
     * ANativeWindow_setFrameRate does. It is applied here rather than natively
     * because only the Activity's window can carry it.
     *
     * <p><b>Measured caveat, so nobody re-derives it:</b> on the nubia NP02J this
     * is ACCEPTED AND IGNORED. Verified independently twice, and the reason
     * appears to be a vendor fps-control stack
     * ({@code ro.vendor.feature.zte_feature_fps_control_*}, which inspects Vulkan
     * command counts and draw calls) sitting above SurfaceFlinger's content
     * detection and re-deciding the rate every frame. On that device the only
     * lever that works is the Settings UI "Screen refresh rate" toggle, which is
     * not reachable from an app. This code is here for hardware WITHOUT that
     * stack, where it is expected to work.
     *
     * <p>{@code debug.dxr.pin_display_mode_hz} = Hz; unset or 0 leaves platform
     * policy alone, which is the default.
     */
    private void pinDisplayModeIfRequested(Activity activity) {
        float wantHz = 0.0f;
        try {
            // Same hidden-API route isTransparentSpikeEnabled() already uses.
            Class<?> sp = Class.forName("android.os.SystemProperties");
            String prop =
                    (String) sp.getMethod("get", String.class)
                            .invoke(null, "debug.dxr.pin_display_mode_hz");
            if (prop != null && !prop.isEmpty()) {
                wantHz = Float.parseFloat(prop);
            }
        } catch (Exception e) {
            // A typo, or no hidden API, reads as "not set" rather than 0 Hz.
            return;
        }
        if (wantHz <= 0.0f) {
            return;
        }
        try {
            Display display = activity.getWindowManager().getDefaultDisplay();
            Display.Mode current = display.getMode();
            Display.Mode best = null;
            for (Display.Mode m : display.getSupportedModes()) {
                // Only ever switch the REFRESH RATE. Matching the current
                // resolution keeps this from silently changing the panel
                // geometry the weave is calibrated against, which would be a
                // far worse bug than the jitter it is trying to fix.
                if (m.getPhysicalWidth() != current.getPhysicalWidth()
                        || m.getPhysicalHeight() != current.getPhysicalHeight()) {
                    continue;
                }
                if (Math.abs(m.getRefreshRate() - wantHz) < 1.0f) {
                    best = m;
                    break;
                }
            }
            if (best == null) {
                Log.w(TAG, "MonadoView: no display mode near " + wantHz + " Hz at the current resolution; leaving platform policy alone");
                return;
            }
            WindowManager.LayoutParams lp = activity.getWindow().getAttributes();
            lp.preferredDisplayModeId = best.getModeId();
            activity.getWindow().setAttributes(lp);
            Log.w(TAG, "MonadoView: requested display mode " + best.getModeId() + " (" + best.getRefreshRate() + " Hz) — VERIFY it held; some vendor builds accept and ignore this");
        } catch (Exception e) {
            // Never let a display-policy nicety take down session creation.
            Log.w(TAG, "MonadoView: display-mode pin failed: " + e);
        }
    }

}
