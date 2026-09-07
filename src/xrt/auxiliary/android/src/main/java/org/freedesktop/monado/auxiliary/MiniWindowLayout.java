// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The measured rules for weaving 1:1 inside an OEM-scaled "mini-window".
 * @author David Fattal
 * @ingroup aux_android_java
 */

package org.freedesktop.monado.auxiliary;

import android.app.Activity;
import android.content.Context;
import android.graphics.Rect;
import android.os.SystemClock;
import android.util.Log;
import androidx.annotation.Keep;
import androidx.annotation.Nullable;

/**
 * ONE implementation of the mini-window 1:1 rules (#1367 S9 / #1277 / #1396).
 *
 * <p>The OEM's "window reply" mini-window does not give the task a smaller window — it gives it a
 * FULL-SIZE logical window and scales the whole task with a SurfaceFlinger leash (measured on
 * NP02J: {@code SCALE TRANSLATE 0.67 @ (1757,236)}), so a 1080x1685 logical window lands on the
 * panel as 723x1129 physical pixels. The vendor interlacer is strict 1:1 buffer→panel: any resample
 * between the woven buffer and the panel destroys the interlace, which is why {@code
 * vk_android_update_container_scaled} degrades such a window to flat 2D.
 *
 * <p>The fix is to make SF's COMPOSED transform identity rather than to fight it: hand the surface
 * a buffer of {@code round(logical * scale)} pixels, and SF's buffer→layer scale times the leash
 * multiplies out to 1.0. The rect then published is the physical one, so the compositor's view
 * dims, the per-window Kooima and the DP's screen origin are all in panel pixels — the same frame
 * the weave happens in.
 *
 * <p>Two callers share this class and MUST share it, because every constant below is a device
 * MEASUREMENT and a second copy would rot:
 *
 * <ul>
 *   <li>{@link MonadoView} — the runtime-spawned {@code _hosted} SurfaceView. It owns the view, so
 *       it applies the layout itself, and it sees the app's touches, so the touch-ratio fallback is
 *       available to it.
 *   <li>the {@code XR_DXR_android_surface_binding} path (#1396) — the app owns its own Surface, so
 *       the runtime can only MEASURE and must hand the answer back as {@code
 *       XrEventDataAndroidWindowLayoutHintDXR}. See {@link #computeHintForActivity}. The runtime
 *       sees none of that app's touch events, so only the vendor API is available there.
 * </ul>
 *
 * <p>TRAP: size the buffer to {@code round(scale * logical)} — which is what the vendor API's Rect
 * and the layer's {@code coveredRegion} both report — and NOT to SurfaceFlinger's {@code
 * displayFrame} (732x1137 here). displayFrame includes the task layer's shadow ({@code
 * shadowRadius} 6 x 0.67 ~ 4 px a side); sizing to it re-introduces an 8 px resample, i.e. exactly
 * the thing this exists to remove.
 */
@Keep
public final class MiniWindowLayout {
    private static final String TAG = "MiniWindowLayout";

    /** A touch drag shorter than this cannot pin a ratio; ignore it. */
    private static final float TOUCH_MIN_SPAN_PX = 40.0f;

    /** Don't re-reflect the vendor API every frame while it keeps failing. */
    private static final long VENDOR_RETRY_MS = 500;

    /** Upper bound on the rationalisation denominator (also bounds the layout search window). */
    private static final int Q_MAX = 250;

    /** How close p/q must sit to a touch-measured scale to be accepted as that scale. */
    private static final float RATIONALISE_TOL = 1.0e-4f;

    /**
     * How far the vendor Rect's two axis ratios may disagree before the scale is refused.
     *
     * <p>HOSTED keeps the v2.16.16 value. It is loose because the Rect is quantised to whole
     * pixels, so a genuinely small mini-window has a real quantisation floor of about {@code 0.5/w
     * + 0.5/h}; tightening it there would silently push such a window onto the 2D fallback, on a
     * path that is shipped and eyeball-approved. See runtime#1399.
     */
    private static final float HOSTED_AXIS_AGREE_TOL = 0.02f;

    /**
     * BINDING is tighter, and only because the surface-binding path can be handed an inconsistent
     * rect that the hosted path cannot (see {@link #isBindingTell}). Measured: a real mini-window
     * gives 0.6704 / 0.6700, 4e-4 apart; the rotation-transient rect gave 0.4525 / 0.4410, 1.15e-2
     * apart — inside the hosted band. Second line of defence behind the transposed reject: a wrong
     * scale here silently weaves the app at the wrong size, while refusing it only costs 2D.
     */
    private static final float BINDING_AXIS_AGREE_TOL = 0.005f;

    /**
     * How far {@code layout * scale} may sit from a whole pixel.
     *
     * <p>Calibration, not a guess: the error David SAW as "a slight double image in BOTH eyes" was
     * 0.4 px (1080 -> 723.6); the shipping 1079 -> 722.93 (0.07 px) and 1685 -> 1128.95 (0.05 px)
     * have been on the panel without a complaint. Both eyes equally is the signature of a residual
     * RESAMPLE (bilinear bleeding adjacent views into each eye), not of a phase error.
     */
    private static final double SNAP_TOL_PX = 0.1;

    // ---- indices into the int[] that computeHintForActivity returns (mirrored in
    // android_mini_window.cpp — keep the two in step).
    public static final int HINT_LAYOUT_W = 0;
    public static final int HINT_LAYOUT_H = 1;
    public static final int HINT_BUFFER_W = 2;
    public static final int HINT_BUFFER_H = 3;
    public static final int HINT_RAT_P = 4;
    public static final int HINT_RAT_Q = 5;
    public static final int HINT_LEN = 6;

    /**
     * Cached {@code debug.dxr.miniwindow_1to1}, -1 = not read yet.
     *
     * <p>VOLATILE (#1401): read from at least two threads — {@link MonadoView} on the UI thread and
     * the surface-binding path on the app's geometry thread. The computation is pure and
     * idempotent, so two threads racing to fill it is harmless and no lock is taken (locking here
     * would put the hosted path behind a monitor it does not need); volatile is what makes the
     * publication safe rather than a data race with an unspecified outcome.
     */
    private static volatile int sPropCached = -1;

    /**
     * Per-process instance for the surface-binding path (one Activity per app process).
     *
     * <p>Volatile plus the {@code synchronized} on its two accessors (#1401): unlike the sysprop,
     * this instance carries the whole per-episode measurement state, so racing to create it could
     * produce TWO instances and silently split the vendor probe and the rationalised p/q between
     * them. {@link MonadoView} does not touch this field — it holds its own instance — so the lock
     * is never on the hosted path.
     */
    @Nullable private static volatile MiniWindowLayout sForActivity = null;

    /** Which of the two bands above this instance uses. */
    private final float axisAgreeTol;

    /** Hosted ({@link MonadoView}) — v2.16.16 behaviour, unchanged. */
    public MiniWindowLayout() {
        this(HOSTED_AXIS_AGREE_TOL);
    }

    private MiniWindowLayout(float axisAgreeTol) {
        this.axisAgreeTol = axisAgreeTol;
    }

    // ---- per-episode state (an "episode" = one entry into a scaled container).
    private int ratP = 0;
    private int ratQ = 0;
    private boolean ratTried = false;
    private boolean inexactLogged = false;
    private int vendorRectW = 0;
    private int vendorRectH = 0;
    private float vendorScale = 0f;
    private long vendorLastTryMs = 0;
    private boolean crossChecked = false;
    private boolean scaleMismatchLogged = false;
    @Nullable private String scaleSource = null;
    private float touchScale = 0f;
    private float touchScaleSpan = 0f;
    private float touchDownRawX = 0f;
    private float touchDownRawY = 0f;
    private float touchDownX = 0f;
    private float touchDownY = 0f;
    private boolean touchDownValid = false;

    /**
     * "This window is being scaled by its container."
     *
     * <p>The window is laid out at its LOGICAL size but placed at its PHYSICAL origin, so a window
     * that is really 0.67x the panel still reports a full-size extent and spills off the edge. One
     * definition, deliberately identical to the compositor's {@code CONTAINER_SCALED} tell.
     *
     * <p>This is the HOSTED predicate and is byte-for-byte what shipped in v2.16.16 — see {@link
     * #isBindingTell} for why the surface-binding path needs a stricter one, and why that
     * strictness is deliberately NOT applied here.
     */
    public static boolean isTell(int x, int y, int w, int h, int dispW, int dispH) {
        return dispW > 0
                && dispH > 0
                && w > 0
                && h > 0
                && (x < 0 || y < 0 || x + w > dispW || y + h > dispH);
    }

    /**
     * {@link #isTell} plus a ROTATION-TRANSIENT REJECT, for the surface-binding path only (#1396).
     *
     * <p>Measured on the NP02J 2026-09-07: an extent that is EXACTLY the panel transposed is not a
     * scaled container, it is a sample taken mid-rotation — the app's {@code getLocationOnScreen}
     * and {@code getRealSize} had updated while its view's width/height had not. Seen as {@code
     * window 1757,236 1600x2560, panel 2560x1600} on the home→recents→freeform path: it spills, so
     * it trips the raw tell, and the vendor Rect over those wrong dimensions yields a
     * plausible-looking 0.4469 that latched a 715x1144 buffer and wove the app at the wrong size. A
     * real scaled container reports the container's own logical size (1080x1685 here), never the
     * panel's transpose.
     *
     * <p>WHY NOT ON THE HOSTED PATH. {@code MonadoView} owns its view, so it never sees this
     * inconsistency in the first place — its width/height and its Display come from the same laid
     * out view. Adding the reject there would only introduce a NEW way for the predicate to flip to
     * false mid-episode (at rotation), and the OFF branch runs {@code restoreLayoutToWindow() +
     * setSizeFromLayout()}: a surface resize under a possibly in-flight weave, which is exactly the
     * class of change that wedged the app in {@code vkWaitForFences} and is why {@link #isEnabled}
     * is cached per process. The hosted path is shipped and eyeball-approved at v2.16.16; it keeps
     * that behaviour untouched. Hardening it needs its own device pass WITH rotation —
     * runtime#1399.
     */
    public static boolean isBindingTell(int x, int y, int w, int h, int dispW, int dispH) {
        if (w == dispH && h == dispW) {
            return false; // mid-rotation sample, not a container scale
        }
        return isTell(x, y, w, h, dispW, dispH);
    }

    /**
     * {@code debug.dxr.miniwindow_1to1} — default ON. Set to 0 to A/B against the honest 2D
     * fallback in {@code vk_android_update_container_scaled}.
     *
     * <p>READ ONCE PER PROCESS and cached, the same contract as {@code debug.dxr.weave_satellite}:
     * changing it mid-run needs an app restart. That is not laziness — flipping it live was
     * measured to WEDGE the app. It re-sizes the surface underneath a weave that is already in
     * flight, and the vendor's {@code leia_cnsdk_weave} then sits forever in {@code
     * vkWaitForFences} inside {@code libleiaCore-impl.so} while the app thread blocks on the
     * compositor mutex in {@code vk_compositor_layer_commit} (ANR trace, NP02J, 2026-09-07). The
     * buffer size may only change where nothing is mid-weave — which on this OEM is the container
     * transition itself, since entering and leaving the mini-window destroys and recreates the
     * surface.
     */
    public static boolean isEnabled() {
        if (sPropCached >= 0) {
            return sPropCached == 1;
        }
        int on = 1;
        try {
            Class<?> sp = Class.forName("android.os.SystemProperties");
            String v =
                    (String)
                            sp.getMethod("get", String.class)
                                    .invoke(null, "debug.dxr.miniwindow_1to1");
            if (v != null && !v.isEmpty()) {
                on =
                        (v.startsWith("0")
                                        || v.startsWith("f")
                                        || v.startsWith("F")
                                        || v.startsWith("n")
                                        || v.startsWith("N"))
                                ? 0
                                : 1;
            }
        } catch (Exception e) {
            on = 1;
        }
        sPropCached = on;
        return on == 1;
    }

    /** Forget everything measured for the previous episode. */
    public void reset() {
        ratP = 0;
        ratQ = 0;
        ratTried = false;
        vendorScale = 0f;
        vendorRectW = 0;
        vendorRectH = 0;
    }

    /** The rationalised scale that {@link #computeSizes} used, or 0 when it had none. */
    public float appliedScale() {
        return ratQ > 0 ? (float) ratP / ratQ : 0f;
    }

    public int ratP() {
        return ratP;
    }

    public int ratQ() {
        return ratQ;
    }

    @Nullable public String scaleSource() {
        return scaleSource;
    }

    /**
     * The container scale, preferring the vendor API and falling back to the touch ratio. When both
     * are available they are cross-checked once: on this firmware they agree exactly (0.670000), so
     * a disagreement means accessibility magnification is multiplied in, or the firmware changed —
     * neither is a number to guess at, so log once and return 0 (stay on the 2D fallback).
     *
     * @return the scale, or 0 when it is not known
     */
    public float resolveScale(@Nullable Activity activity, int w, int h, int dispW, int dispH) {
        float api = queryVendorWrScale(activity, w, h, dispW, dispH);
        float touch = touchScale;

        if (api > 0f && touch > 0f) {
            if (Math.abs(api - touch) > 0.01f) {
                if (!scaleMismatchLogged) {
                    scaleMismatchLogged = true;
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
            if (!crossChecked) {
                crossChecked = true;
                Log.i(
                        TAG,
                        "miniWindow1to1: cross-check OK, vendor-api " + api + " == touch " + touch);
            }
        }

        if (api > 0f) {
            scaleSource = "vendor-api";
            return api;
        }
        if (touch > 0f) {
            scaleSource = "touch";
            return touch;
        }
        return 0f;
    }

    /**
     * The exact-integer sizing.
     *
     * <p>{@code round(logical * s)} is NOT enough. 1080 * 0.67 = 723.6 rounds to 724, so SF
     * composes buffer->layer x leash = (1080/724) x 0.67 = 0.9994 — a 0.06 % scale, ~0.4 px of
     * drift across a 724 px window, which reads as a slight double image in BOTH eyes.
     *
     * <p>The requirement is NOT "a multiple of q" — that was too blunt, it cost a 54 px (5 %) black
     * border on the right and another 57 px at the bottom, which David rejected. All that is
     * actually required is that {@code layout * s} land close ENOUGH to an integer, so search for
     * the LARGEST layout within q of the window whose residual is under {@link #SNAP_TOL_PX}. With
     * s = 0.67 that is 1079 wide (722.93, e = 0.07 px -> buffer 723) and the FULL 1685 high
     * (1128.95, e = 0.05 px -> buffer 1129). The border collapses from 54 px to ONE logical pixel.
     *
     * <p>What does NOT work, measured on device, so do not re-try it: OVERSCANNING (a layout LARGER
     * than the window, so the crop eats the remainder). A SurfaceView bigger than its window has
     * its surface sized to the VISIBLE frame, so a 737x1139 buffer got mapped into 723.6x1128.95
     * screen px — composed 0.982, a 2 % resample, and the double image came straight back.
     *
     * @param out receives {layoutW, layoutH, bufferW, bufferH} at {@link #HINT_LAYOUT_W}..
     * @return true when a usable layout was produced
     */
    public boolean computeSizes(float s, int w, int h, int[] out) {
        if (s <= 0f || w <= 0 || h <= 0 || out == null || out.length < 4) {
            return false;
        }
        if (!rationaliseScale(s, w, h)) {
            // No usable p/q: fall back to the old rounded size, which still beats the
            // 2D degrade, and say so once.
            if (!inexactLogged) {
                inexactLogged = true;
                Log.w(
                        TAG,
                        "miniWindow1to1: scale "
                                + s
                                + " does not rationalise to a usable p/q (q<="
                                + Q_MAX
                                + ") — exact 1:1 is NOT reachable, using round(logical*s);"
                                + " expect a sub-pixel resample");
            }
            ratP = 0;
            ratQ = 0;
        }

        int layoutW;
        int layoutH;
        int bw;
        int bh;
        if (ratQ > 0) {
            layoutW = snapDimension(w, ratP, ratQ);
            layoutH = snapDimension(h, ratP, ratQ);
            bw = (int) Math.round((double) layoutW * ratP / ratQ);
            bh = (int) Math.round((double) layoutH * ratP / ratQ);
        } else {
            layoutW = w;
            layoutH = h;
            bw = Math.round(w * s);
            bh = Math.round(h * s);
        }
        if (layoutW <= 0 || layoutH <= 0 || bw <= 0 || bh <= 0) {
            return false;
        }
        out[HINT_LAYOUT_W] = layoutW;
        out[HINT_LAYOUT_H] = layoutH;
        out[HINT_BUFFER_W] = bw;
        out[HINT_BUFFER_H] = bh;
        return true;
    }

    /**
     * Sub-pixel drift the composition still carries, over the whole window, in panel px.
     *
     * <p>Computed from the RATIONALISED p/q, not from the raw measured float — p/q is what the
     * sizing used and what the leash actually is; the float carries the vendor Rect's whole-pixel
     * quantisation and would overstate the drift by 2-4x.
     */
    public double residualPx(int layout, int buffer) {
        if (ratQ <= 0) {
            return 0.0;
        }
        return Math.abs((double) layout * ratP / ratQ - buffer);
    }

    /**
     * The largest layout size within q of {@code dim} whose on-screen extent ({@code layout * p/q})
     * is within {@link #SNAP_TOL_PX} of a whole pixel — i.e. the smallest border that still buys a
     * resample-free composition.
     *
     * <p>An exact multiple of q always satisfies this with residual 0 and is always inside the
     * search window ({@code dim - dim%q}, and {@code dim%q < q}), so the loop cannot come back
     * empty; the return after it only covers a degenerate {@code dim < q}.
     */
    private static int snapDimension(int dim, int p, int q) {
        int lo = Math.max(1, dim - q);
        for (int l = dim; l >= lo; l--) {
            double v = (double) l * p / q;
            if (Math.abs(v - Math.rint(v)) <= SNAP_TOL_PX) {
                return l;
            }
        }
        int mult = (dim / q) * q;
        return mult > 0 ? mult : dim;
    }

    /**
     * Rationalise the measured scale to {@code p/q}.
     *
     * <p>q = 100 IS TRIED FIRST, and that is a deliberate prior, not a shortcut: an OEM window
     * scale is a round percentage (this one is 0.67, and SurfaceFlinger prints the leash as {@code
     * 0.6700}). Smallest-q-wins on its own picks the WRONG fraction here — the vendor Rect only
     * pins the scale to about 3e-4 because it is quantised to whole pixels, and inside that band
     * {@code 63/94 = 0.670213} both reproduces the Rect exactly AND has a smaller q than {@code
     * 67/100}, while composing to 0.99968 instead of 1. So try the percentage first, and only fall
     * back to an ascending search for a device that is not on a percentage grid.
     *
     * @return true when a usable p/q was found
     */
    private boolean rationaliseScale(float s, int w, int h) {
        if (ratQ > 0) {
            return true; // resolved once per scaled-container episode
        }
        if (ratTried) {
            return false; // already searched and failed; don't re-search every frame
        }
        ratTried = true;
        int p100 = Math.round(s * 100f);
        if (p100 > 0 && acceptRational(p100, 100, s, w, h)) {
            ratP = p100;
            ratQ = 100;
            return true;
        }
        for (int q = 1; q <= Q_MAX; q++) {
            int p = Math.round(s * q);
            if (p <= 0) {
                continue;
            }
            if (acceptRational(p, q, s, w, h)) {
                ratP = p;
                ratQ = q;
                return true;
            }
        }
        return false;
    }

    /**
     * A p/q is usable when it explains the evidence we actually have, and when snapping the layout
     * to a multiple of q does not move it far from the window.
     */
    private boolean acceptRational(int p, int q, float s, int w, int h) {
        if (q <= 0 || w <= 0 || h <= 0) {
            return false;
        }
        // q only bounds how far snapDimension may search; the border it actually costs is
        // whatever that search finds (one logical px here). Keep the window under a tenth
        // of the axis so the worst case stays bounded too.
        if ((q - 1) * 10 > w) {
            return false;
        }
        if (vendorRectW > 0 && vendorRectH > 0) {
            return Math.round((double) w * p / q) == vendorRectW
                    && Math.round((double) h * p / q) == vendorRectH;
        }
        return Math.abs(s - (float) p / q) <= RATIONALISE_TOL;
    }

    /**
     * S9 probe result (b): {@code ActivityManager.getDefaultWindowParamByTaskForNormalWr(taskId)}
     * is a hidden TEST-API that is NOT on this build's blocklist and returns the POST-SCALE
     * on-screen Rect for an ordinary app uid, no permission needed.
     *
     * <p>MUST BE GATED ON THE TELL by the caller, and that gate is load-bearing: the method returns
     * the NOMINAL window-reply placement whether or not the app is actually in a mini-window — in
     * fullscreen it still answers {@code Rect(1757,236-2481,1365)}. An ungated read would shrink a
     * fullscreen buffer to 724x1129.
     *
     * @return the scale, or 0 when unavailable or implausible
     */
    private float queryVendorWrScale(
            @Nullable Activity activity, int w, int h, int dispW, int dispH) {
        if (vendorScale > 0f) {
            return vendorScale; // resolved once per scaled-container episode
        }
        long now = SystemClock.uptimeMillis();
        if (now - vendorLastTryMs < VENDOR_RETRY_MS) {
            return 0f; // don't re-reflect every frame while it keeps failing
        }
        vendorLastTryMs = now;

        if (activity == null || w <= 0 || h <= 0) {
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
                                        .getMethod(
                                                "getDefaultWindowParamByTaskForNormalWr", int.class)
                                        .invoke(am, activity.getTaskId());
            } catch (Throwable ignored) {
                r = null;
            }
            if (r == null || r.width() <= 0 || r.height() <= 0) {
                try {
                    r =
                            (Rect)
                                    am.getClass()
                                            .getMethod(
                                                    "getDefaultWindowParamForNormalWr",
                                                    boolean.class)
                                            .invoke(am, Boolean.FALSE);
                } catch (Throwable ignored) {
                    return 0f;
                }
            }
            if (r == null || r.width() <= 0 || r.height() <= 0) {
                return 0f;
            }
            // A result that does not fit the panel is not an on-screen rect.
            if (dispW > 0 && dispH > 0 && (r.width() > dispW || r.height() > dispH)) {
                return 0f;
            }
            float sx = r.width() / (float) w;
            float sy = r.height() / (float) h;
            // One leash scales both axes, so the two ratios must agree. How tightly is
            // per-instance: see HOSTED_AXIS_AGREE_TOL / BINDING_AXIS_AGREE_TOL.
            if (sx < 0.2f || sx > 1.0f || Math.abs(sx - sy) > axisAgreeTol) {
                return 0f;
            }
            // Keep the RAW rect: the rationalisation round-trips against these integers
            // rather than against the lossy float, which is the only evidence that
            // actually pins the scale.
            vendorRectW = r.width();
            vendorRectH = r.height();
            vendorScale = (sx + sy) * 0.5f;
            return vendorScale;
        } catch (Throwable t) {
            return 0f;
        }
    }

    /**
     * S9 probe result (c): the platform inverts the leash on the way in, so on a REAL dispatched
     * drag {@code |Δraw| / |Δlocal|} is the container scale (measured 0.670000, max residual 1e-4
     * px over 100 samples), and it needs neither a vendor API nor a permission. Differences, so the
     * view position and the insets cancel. Synthetic events ({@code MotionEvent.obtain}) read 1.0
     * and are rejected by the plausibility band below, as is a fullscreen window.
     *
     * <p>Only {@link MonadoView} can feed this: it is the app's touch-receiving view. The
     * surface-binding path (#1396) never sees the app's MotionEvents, so it is vendor-API only —
     * unless the app itself measures the ratio and forwards it, which the spec leaves open.
     */
    public void measureTouchScale(android.view.MotionEvent ev) {
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
        float rawSpan =
                Math.abs(ev.getRawX() - touchDownRawX) + Math.abs(ev.getRawY() - touchDownRawY);
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
     * The surface-binding entry point (#1396), called from native ({@code android_mini_window.cpp})
     * on every {@code xrSetAndroidWindowGeometryDXR} whose rect trips the tell. The app owns the
     * view, so this ONLY measures — the answer travels back to the app as {@code
     * XrEventDataAndroidWindowLayoutHintDXR} and the app applies it.
     *
     * <p>{@code activityObj} is the {@code XrInstanceCreateInfoAndroidKHR::applicationActivity} the
     * runtime already holds. Everything the probe needs is on it (an ActivityManager and a task
     * id); nothing here touches the app's view hierarchy.
     *
     * @return {@link #HINT_LEN} ints — layoutW, layoutH, bufferW, bufferH, p, q — or null when the
     *     window fits the panel, the feature is off, or the scale is not knowable on this device.
     */
    @Keep
    @Nullable public static synchronized int[] computeHintForActivity(
            @Nullable Object activityObj, int x, int y, int w, int h, int dispW, int dispH) {
        if (!isBindingTell(x, y, w, h, dispW, dispH) || !isEnabled()) {
            return null;
        }
        if (!(activityObj instanceof Activity)) {
            return null;
        }
        if (!isInScalableContainer(activityObj)) {
            return null; // a fullscreen task is never container-scaled
        }
        Activity activity = (Activity) activityObj;
        MiniWindowLayout self = sForActivity;
        if (self == null) {
            self = new MiniWindowLayout(BINDING_AXIS_AGREE_TOL);
            sForActivity = self;
        }
        float s = self.resolveScale(activity, w, h, dispW, dispH);
        if (s <= 0f) {
            return null;
        }
        int[] out = new int[HINT_LEN];
        if (!self.computeSizes(s, w, h, out)) {
            return null;
        }
        out[HINT_RAT_P] = self.ratP;
        out[HINT_RAT_Q] = self.ratQ;
        return out;
    }

    /**
     * Is this Activity still in a container that can scale it? (#1396)
     *
     * <p>The surface-binding latch needs this and cannot derive it. Once an app has applied a hint
     * it publishes the PHYSICAL rect, which by construction fits the panel — so "the window left
     * its container" and "the hint is working" look identical from the rect alone. MEASURED: after
     * a task is moved back to fullscreen the app's window keeps the hinted layout, so the rect it
     * publishes still fits and the hint stayed latched, leaving a 723x1129 window weaving in a
     * fullscreen task.
     *
     * <p>{@code Activity.isInMultiWindowMode()} is the exact, PUBLIC answer: true for the OEM
     * mini-window (windowing mode freeform) and for split-screen, false for fullscreen.
     * Split-screen never trips the tell in the first place (the window fits), so "multi-window" is
     * a safe superset of "possibly scaled".
     *
     * <p>Thread-safe to call off the UI thread, which the geometry channel is. NOT free on every
     * API level: {@code Activity.isInMultiWindowMode()} only became a cached field in API 28 — on
     * 24-27 it is a binder round trip to ActivityTaskManager. It is called once per published
     * geometry change (not per frame), which is a handful of calls per container transition, so
     * that is affordable; do not move it into a per-frame path.
     *
     * @return true when the Activity is in a multi-window container, or when it cannot be asked
     *     (never end a hint on ignorance — the tell will).
     */
    @Keep
    public static boolean isInScalableContainer(@Nullable Object activityObj) {
        if (!(activityObj instanceof Activity)) {
            return true;
        }
        try {
            return ((Activity) activityObj).isInMultiWindowMode();
        } catch (Throwable t) {
            return true;
        }
    }

    /**
     * Forget the surface-binding episode state — the window left its scaled container.
     *
     * <p>{@code synchronized} on the same monitor as {@link #computeHintForActivity} (#1401): the
     * two are called from different native threads (the geometry channel and session teardown), and
     * a reset that interleaves with a compute would half-clear the episode.
     */
    @Keep
    public static synchronized void resetForActivity() {
        MiniWindowLayout self = sForActivity;
        if (self != null) {
            self.reset();
        }
    }
}
