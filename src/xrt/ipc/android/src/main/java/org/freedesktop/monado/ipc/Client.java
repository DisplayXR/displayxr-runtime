// Copyright 2020, Collabora, Ltd.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Implementation of the Monado AIDL server
 * @author Rylie Pavlik <rylie.pavlik@collabora.com>
 * @ingroup ipc_android
 */

package org.freedesktop.monado.ipc;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.ServiceConnection;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageManager;
import android.os.Binder;
import android.os.Build;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import android.util.Log;
import android.view.Surface;
import android.view.SurfaceHolder;
import androidx.annotation.Keep;
import androidx.annotation.Nullable;
import java.io.IOException;
import java.util.concurrent.Executors;
import org.freedesktop.monado.auxiliary.MonadoView;
import org.freedesktop.monado.auxiliary.NativeCounterpart;
import org.freedesktop.monado.auxiliary.SystemUiController;

/**
 * Provides the client-side code to initiate connection to Monado IPC service.
 *
 * <p>This class will get loaded into the OpenXR client application by our native code.
 */
@Keep
public class Client implements ServiceConnection {
    private static final String TAG = "monado-ipc-client";

    /** Optional client-manifest pin for the satellite compositor slot (#1031). */
    private static final String SLOT_META_DATA = "com.displayxr.satellite_slot";

    /** Dev override for the satellite slot, read before the manifest pin. */
    private static final String SLOT_SYSPROP = "debug.dxr.slot";

    /** How long to wait for the slot broker before falling back to the main-process service. */
    private static final long BROKER_TIMEOUT_MS = 3000;

    /** Explicit component of the main-process broker (see SlotBrokerService). */
    private static final String SLOT_BROKER_CLASS_NAME =
            "org.freedesktop.monado.ipc.SlotBrokerService";

    /** Used to block until binder is ready. */
    private final Object binderSync = new Object();

    /** Keep track of the ipc_client_android instance over on the native side. */
    private final NativeCounterpart nativeCounterpart;

    /**
     * Pointer to local IPC proxy: calling methods on it automatically transports arguments across
     * binder IPC.
     *
     * <p>May be null!
     */
    @Keep public IMonado monado = null;

    /**
     * Indicates that we tried to connect but failed.
     *
     * <p>Used to distinguish a "not yet fully connected" null monado member from a "tried and
     * failed" null monado member.
     */
    @Keep public boolean failed = false;

    /**
     * "Our" side of the socket pair - the other side is sent to the server automatically on
     * connection.
     */
    private ParcelFileDescriptor fd = null;

    /** Context provided by app. */
    private Context context = null;

    /** Context of the runtime package */
    private Context runtimePackageContext = null;

    /** Control system ui visibility */
    private SystemUiController systemUiController = null;

    /** The view we injected into the client activity; retained to observe its surface (#528). */
    private MonadoView monadoView = null;

    /** Guards lastSurfaceSent. */
    private final Object surfaceSync = new Object();

    /**
     * The last Surface forwarded to the service via passAppSurface, for dedupe. Nulled on surface
     * loss so the next available surface is always re-sent, even if the platform reuses the
     * Surface object.
     */
    private Surface lastSurfaceSent = null;

    /** Connection to the main-process slot broker; held for as long as we hold a slot (#1031). */
    private final BrokerConnection brokerConnection = new BrokerConnection();

    /**
     * The token the broker links to death on. Its lifetime IS our claim on the slot: if this
     * process dies without releasing, the broker frees the slot when this binder dies with it.
     */
    private final IBinder slotToken = new Binder();

    /** Satellite slot we hold, or -1 when we are on the classic main-process service. */
    private int satelliteSlot = -1;

    /**
     * Constructor
     *
     * @param nativePointer the corresponding native object's pointer.
     */
    @Keep
    public Client(long nativePointer) {
        this.nativeCounterpart = new NativeCounterpart(nativePointer);
        this.nativeCounterpart.markAsUsedByNativeCode();
    }

    private void shutdown() {
        monado = null;
        if (context != null) {
            context.unbindService(this);
        }
        // Give the satellite back before we stop existing, so the next app can have it
        // without waiting for our binder death to be noticed.
        releaseSatelliteSlot();

        if (fd != null) {
            try {
                fd.close();
            } catch (IOException e) {
                e.printStackTrace();
            }
            fd = null;
        }
    }

    /** Let the native code notify us that it is no longer using this class. */
    @Keep
    public void markAsDiscardedByNative() {
        nativeCounterpart.markAsDiscardedByNative(TAG);
        shutdown();
    }

    /**
     * Bind to the Monado IPC service, and block until it is fully connected.
     *
     * <p>The IPC client code on Android should load this class (from the right package),
     * instantiate this class (retaining a reference to it!), and call this method.
     *
     * <p>This method must not be called from the main (UI) thread.
     *
     * @param context_ Context to use to make the connection. (We get the application context from
     *     it.)
     * @param packageName The package name containing the Monado runtime. The caller is guaranteed
     *     to know this because it had to load this class from that package. There's a define in
     *     xrt_config_android.h to use for this.
     * @return the fd number - do not close! (dup if you want to be able to close it) Returns -1 if
     *     something went wrong.
     *     <p>Various builds, variants, etc. will have different package names, but we must specify
     *     the package name explicitly to avoid violating security restrictions.
     */
    @Keep
    public int blockingConnect(Context context_, String packageName) {
        Log.i(TAG, "blockingConnect");

        synchronized (binderSync) {
            if (!bind(context_, packageName)) {
                Log.e(TAG, "Bind failed immediately");
                // Bind failed immediately
                return -1;
            }
            try {
                binderSync.wait();
            } catch (InterruptedException e) {
                Log.e(TAG, "Interrupted: " + e);
                return -1;
            }
        }

        if (monado == null) {
            Log.e(TAG, "Invalid binder object");
            return -1;
        }

        // This block of code asynchronously create a MonadoView attached to activity and
        // waits for Surface creation. Native code (comp_window_android_init_swapchain() method)
        // will poll for ANativeWindow created from this Surface.
        // TODO: just initiate MonadoView attachment and add callback to native code to
        // notify about Surface status and pass it to OpenXR application as a Session lifecycle
        // (ready ... synchronized ... visible ... focused)
        new Thread(
                        () -> {
                            Activity activity = null;
                            if (context_ instanceof Activity) {
                                activity = (Activity) context_;
                            }

                            try {
                                if (!monado.canDrawOverOtherApps() && activity != null) {
                                    Surface surface = attachViewAndGetSurface(activity);
                                    if (surface == null) {
                                        Log.e(TAG, "Failed to create surface");
                                        handleFailure();
                                        return;
                                    }

                                    sendSurfaceIfNew(surface);
                                }
                            } catch (RemoteException e) {
                                e.printStackTrace();
                            }

                            if (activity != null) {
                                systemUiController =
                                        new SystemUiController(activity.getWindow().getDecorView());
                                systemUiController.hide();
                            }
                        })
                .start();

        // Create socket pair
        ParcelFileDescriptor theirs;
        ParcelFileDescriptor ours;
        try {
            ParcelFileDescriptor[] fds = ParcelFileDescriptor.createSocketPair();
            ours = fds[0];
            theirs = fds[1];
            monado.connect(theirs);
        } catch (IOException e) {
            e.printStackTrace();
            Log.e(TAG, "could not create socket pair: " + e);
            handleFailure();
            return -1;
        } catch (RemoteException e) {
            e.printStackTrace();
            Log.e(TAG, "could not connect to service: " + e);
            handleFailure();
            return -1;
        }

        fd = ours;
        Log.i(TAG, "Socket fd " + fd.getFd());
        return fd.getFd();
    }

    /**
     * Bind to the Monado IPC service - this asynchronously starts connecting (and launching the
     * service if it's not already running)
     *
     * @param context_ Context to use to make the connection. (We get the application context from
     *     it.)
     * @param packageName The package name containing the Monado runtime. The caller is guaranteed
     *     to know this because it had to load this class from that package. There's a define in
     *     xrt_config_android.h to use for this.
     *     <p>Various builds, variants, etc. will have different package names, but we must specify
     *     the package name explicitly to avoid violating security restrictions.
     */
    public boolean bind(Context context_, String packageName) {
        Log.i(TAG, "bind");
        context = context_.getApplicationContext();
        if (context == null) {
            // in case app context returned null
            context = context_;
        }
        try {
            runtimePackageContext =
                    context.createPackageContext(
                            packageName,
                            Context.CONTEXT_IGNORE_SECURITY | Context.CONTEXT_INCLUDE_CODE);
        } catch (PackageManager.NameNotFoundException e) {
            e.printStackTrace();
            Log.e(TAG, "bind: Could not find package " + packageName);
            return false;
        }

        // ADR-036 D3 / #1031: an OpenXR client gets its own SATELLITE compositor process
        // (:dxrN) rather than sharing the single main-process service, because the pieces
        // that decide what ends up on the panel — the app surface, the display processor,
        // the vendor core — are process-global in the runtime. One client per process is
        // what makes N apps weave at once.
        //
        // The runtime's OWN in-APK clients (the dashboard's headless diag query) stay on the
        // main-process service: they are short-lived, never present, and would otherwise
        // occupy a satellite that a real app needs.
        Intent intent;
        if (packageName.equals(context.getPackageName())) {
            intent = new Intent(BuildConfig.SERVICE_ACTION).setPackage(packageName);
            Log.i(TAG, "bind: in-APK client — using the main-process MonadoService");
        } else {
            satelliteSlot = acquireSatelliteSlot(context_, packageName);
            if (satelliteSlot < 0) {
                // Not fatal: without a satellite the runtime behaves exactly as it did
                // before slots existed — one client on screen, on the main-process service.
                intent = new Intent(BuildConfig.SERVICE_ACTION).setPackage(packageName);
                Log.w(TAG, "bind: no satellite slot — falling back to the main-process service");
            } else {
                intent =
                        new Intent(BuildConfig.SERVICE_ACTION)
                                .setComponent(
                                        new ComponentName(
                                                packageName,
                                                MonadoServiceSlots.classNameFor(satelliteSlot)));
                Log.i(
                        TAG,
                        "bind: satellite slot "
                                + satelliteSlot
                                + " ("
                                + intent.getComponent().getClassName()
                                + ") for "
                                + context.getPackageName());
            }
        }

        if (!bindService(context, intent)) {
            Log.e(TAG, "bindService: Service " + intent + " could not be found to bind!");
            releaseSatelliteSlot();
            return false;
        }

        // does not bind right away! This takes some time.
        return true;
    }

    /**
     * Ask the runtime's main-process broker for a satellite compositor slot (#1031).
     *
     * <p>Blocking, and deliberately so: {@link #blockingConnect} is already documented as
     * off-the-UI-thread, and the slot has to be known before we can name the component to bind. A
     * broker that does not answer within {@link #BROKER_TIMEOUT_MS} is treated as "no slot", which
     * costs concurrency and nothing else.
     *
     * <p>The broker binding is kept afterwards, not dropped: it is what pins the assignment (the
     * broker holds a death link on {@link #slotToken}) and what keeps the broker process at this
     * app's importance.
     *
     * @return the slot index, or -1 to use the main-process service.
     */
    private int acquireSatelliteSlot(Context context_, String packageName) {
        Intent intent =
                new Intent().setComponent(new ComponentName(packageName, SLOT_BROKER_CLASS_NAME));
        ISlotBroker broker = brokerConnection.connect(context, intent);
        if (broker == null) {
            Log.w(TAG, "acquireSatelliteSlot: slot broker unavailable");
            return -1;
        }
        try {
            int slot =
                    broker.acquireSlot(
                            context_.getPackageName(),
                            android.os.Process.myPid(),
                            preferredSlot(context_),
                            slotToken);
            if (slot < 0) {
                Log.w(TAG, "acquireSatelliteSlot: broker has no free slot");
            }
            return slot;
        } catch (RemoteException e) {
            Log.e(TAG, "acquireSatelliteSlot: " + e);
            return -1;
        }
    }

    /** Hand the slot back so the next app can have it. Safe to call when we never had one. */
    private void releaseSatelliteSlot() {
        ISlotBroker broker = brokerConnection.get();
        if (broker != null && satelliteSlot >= 0) {
            try {
                broker.releaseSlot(satelliteSlot, slotToken);
            } catch (RemoteException e) {
                // The broker died; it frees the slot on our token's death anyway.
            }
        }
        satelliteSlot = -1;
        brokerConnection.disconnect(context);
    }

    /**
     * A slot the client would like, or -1. {@code setprop debug.dxr.slot N} is the dev override
     * (test harnesses pin a known slot); the {@code com.displayxr.satellite_slot} manifest
     * meta-data is an app pin. Both are only a *preference* — the broker still owns the decision,
     * so two apps pinning the same slot cannot collide.
     */
    private static int preferredSlot(Context context_) {
        int override = getIntSystemProperty(SLOT_SYSPROP, -1);
        if (override >= 0) {
            Log.i(TAG, "preferredSlot: " + SLOT_SYSPROP + " -> " + override);
            return override;
        }
        int declared = getAppMetaDataInt(context_, SLOT_META_DATA, -1);
        if (declared >= 0) {
            Log.i(TAG, "preferredSlot: " + SLOT_META_DATA + " -> " + declared);
        }
        return declared;
    }

    /** Manifest {@code <meta-data>} on the client app. */
    private static int getAppMetaDataInt(Context context_, String key, int defaultValue) {
        try {
            ApplicationInfo ai =
                    context_.getPackageManager()
                            .getApplicationInfo(
                                    context_.getPackageName(), PackageManager.GET_META_DATA);
            if (ai.metaData == null) {
                return defaultValue;
            }
            Object raw = ai.metaData.get(key);
            if (raw instanceof Integer) {
                return (Integer) raw;
            }
            if (raw instanceof String) {
                return Integer.parseInt(((String) raw).trim());
            }
            return defaultValue;
        } catch (Exception e) {
            return defaultValue;
        }
    }

    /** Read an {@code android.os.SystemProperties} int by reflection (it is a hidden API). */
    private static int getIntSystemProperty(String key, int defaultValue) {
        try {
            Class<?> sp = Class.forName("android.os.SystemProperties");
            String raw = (String) sp.getMethod("get", String.class).invoke(null, key);
            if (raw == null || raw.isEmpty()) {
                return defaultValue;
            }
            return Integer.parseInt(raw.trim());
        } catch (Exception e) {
            return defaultValue;
        }
    }

    /** Bind/unbind helper for the slot broker, with a bounded wait for onServiceConnected. */
    private class BrokerConnection implements ServiceConnection {
        private final Object sync = new Object();
        private ISlotBroker broker = null;
        private boolean bound = false;
        private boolean settled = false;

        @Nullable
        ISlotBroker connect(Context ctx, Intent intent) {
            synchronized (sync) {
                if (broker != null) {
                    return broker;
                }
                if (!bound) {
                    settled = false;
                    // Same flags as the compositor bind: the broker must inherit our
                    // importance, or the freezer can take it out from under us.
                    bound = bindService(ctx, intent, this);
                    if (!bound) {
                        return null;
                    }
                }
                long deadline = android.os.SystemClock.uptimeMillis() + BROKER_TIMEOUT_MS;
                while (!settled) {
                    long remaining = deadline - android.os.SystemClock.uptimeMillis();
                    if (remaining <= 0) {
                        Log.w(TAG, "slot broker did not connect within " + BROKER_TIMEOUT_MS + "ms");
                        break;
                    }
                    try {
                        sync.wait(remaining);
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        break;
                    }
                }
                return broker;
            }
        }

        @Nullable
        ISlotBroker get() {
            synchronized (sync) {
                return broker;
            }
        }

        void disconnect(Context ctx) {
            synchronized (sync) {
                // Drop the IBinder as well as the binding: a sync binder call into a frozen
                // process kills it, so nothing may keep a reference to a service it no
                // longer holds a binding to.
                broker = null;
                if (bound && ctx != null) {
                    try {
                        ctx.unbindService(this);
                    } catch (IllegalArgumentException e) {
                        // Never bound / already unbound.
                    }
                    bound = false;
                }
            }
        }

        @Override
        public void onServiceConnected(ComponentName name, IBinder service) {
            synchronized (sync) {
                broker = ISlotBroker.Stub.asInterface(service);
                settled = true;
                sync.notifyAll();
            }
        }

        @Override
        public void onServiceDisconnected(ComponentName name) {
            synchronized (sync) {
                broker = null;
                settled = true;
                sync.notifyAll();
            }
        }
    }

    private boolean bindService(Context context, Intent intent) {
        return bindService(context, intent, this);
    }

    /**
     * Bind with the flags that make the target inherit OUR importance.
     *
     * <p>{@code BIND_IMPORTANT | BIND_ABOVE_CLIENT | BIND_INCLUDE_CAPABILITIES} is what keeps a
     * satellite (and the broker) out of the freezer for as long as this app is foreground, and
     * makes them freeze WITH the app when it is cached — which is exactly the desired behaviour
     * for a compositor that exists to serve one window.
     */
    private boolean bindService(Context context, Intent intent, ServiceConnection connection) {
        boolean result;
        int flags = Context.BIND_AUTO_CREATE | Context.BIND_IMPORTANT | Context.BIND_ABOVE_CLIENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            result =
                    context.bindService(
                            intent,
                            flags | Context.BIND_INCLUDE_CAPABILITIES,
                            Executors.newSingleThreadExecutor(),
                            connection);
        } else {
            result = context.bindService(intent, connection, flags);
        }

        return result;
    }

    /** Some on-failure cleanup. */
    private void handleFailure() {
        failed = true;
        shutdown();
    }

    @Nullable private Surface attachViewAndGetSurface(Activity activity) {
        // Observe the surface lifecycle so a background→resume cycle (e.g. a SAF
        // file picker) forwards the destroy + the NEW surface to the service —
        // otherwise the service compositor keeps presenting into the abandoned
        // BufferQueue and the panel freezes (#528).
        monadoView =
                MonadoView.attachToActivity(
                        activity,
                        new MonadoView.SurfaceStateListener() {
                            @Override
                            public void onSurfaceAvailable(SurfaceHolder holder) {
                                sendSurfaceIfNew(holder.getSurface());
                            }

                            @Override
                            public void onSurfaceDestroyed() {
                                notifySurfaceLost();
                            }

                            @Override
                            public void onWindowRectChanged(
                                    int x, int y, int w, int h, int displayId, int dispW,
                                    int dispH) {
                                sendWindowRect(x, y, w, h, displayId, dispW, dispH);
                            }
                        });
        SurfaceHolder holder = monadoView.waitGetSurfaceHolder(2000);
        Surface surface = null;
        if (holder != null) {
            surface = holder.getSurface();
        }

        return surface;
    }

    /**
     * Forward the surface to the service unless it is the one we already sent.
     *
     * <p>Identity dedupe is enough: a size-only surfaceChanged keeps the same Surface, and the
     * service tracks extent changes itself by polling the surface caps each acquire (#510).
     * Called from both the blockingConnect worker thread and the UI-thread surface callbacks.
     */
    private void sendSurfaceIfNew(@Nullable Surface surface) {
        synchronized (surfaceSync) {
            if (surface == null || !surface.isValid() || surface == lastSurfaceSent) {
                return;
            }
            IMonado service = monado;
            if (service == null) {
                return;
            }
            try {
                service.passAppSurface(surface);
                lastSurfaceSent = surface;
                Log.i(TAG, "passAppSurface: forwarded new surface to service (#528)");
            } catch (RemoteException e) {
                Log.e(TAG, "passAppSurface failed: " + e);
            }
        }
    }

    /**
     * Forward this window's on-screen rect to the service so the per-session compositor can anchor
     * the weave phase where the window physically sits on the panel (ADR-036 D6, #1033).
     *
     * <p>{@code oneway} on the AIDL side: never blocks the UI thread, and a dropped update
     * self-heals on the next change. Fired only on an actual change (MonadoView dedupes), so a
     * static window costs zero binder traffic.
     *
     * <p>Best-effort before the binder is up — MonadoView starts polling at
     * {@code onAttachedToWindow}, which can precede {@code onServiceConnected}. The first rect
     * after connect is re-sent because the last-sent cache lives in the service, not here, and the
     * view re-reports on its next real change; the compositor also falls back to display-scoped
     * weaving until a rect arrives.
     */
    private void sendWindowRect(int x, int y, int w, int h, int displayId, int dispW, int dispH) {
        IMonado service = monado;
        if (service == null) {
            return;
        }
        try {
            service.updateWindowRect(x, y, w, h, displayId, dispW, dispH);
        } catch (RemoteException e) {
            Log.e(TAG, "updateWindowRect failed: " + e);
        }
    }

    /**
     * Tell the service the surface is gone so its compositor stops presenting into the dead
     * BufferQueue and tears its VkSurfaceKHR down (#528).
     *
     * <p>Synchronous binder call on purpose: returning from surfaceDestroyed is what invalidates
     * the surface, so the service's generation bump must land before that.
     */
    private void notifySurfaceLost() {
        synchronized (surfaceSync) {
            // Null first: guarantees the next available surface is re-sent even if
            // the platform hands back the same Surface object on resume.
            lastSurfaceSent = null;
            IMonado service = monado;
            if (service == null) {
                return;
            }
            try {
                service.clearAppSurface();
                Log.i(TAG, "clearAppSurface: notified service of surface loss (#528)");
            } catch (RemoteException e) {
                Log.e(TAG, "clearAppSurface failed: " + e);
            }
        }
    }

    /**
     * Handle the asynchronous connection of the binder IPC.
     *
     * @param name should match the preceding intent, but not used.
     * @param service the associated service, which we cast in this function.
     */
    @Override
    public void onServiceConnected(ComponentName name, IBinder service) {
        Log.i(TAG, "onServiceConnected");

        synchronized (binderSync) {
            monado = IMonado.Stub.asInterface(service);
            binderSync.notify();
        }
    }

    /**
     * Handle asynchronous disconnect.
     *
     * @param name should match the preceding intent, but not used.
     */
    @Override
    public void onServiceDisconnected(ComponentName name) {
        Log.i(TAG, "onServiceDisconnected");
        shutdown();
        // ! @todo tell C/C++ that the world is crumbling, then close the fd here.
    }

    /*
     * @todo do we need to watch for a disconnect here?
     *   https://stackoverflow.com/questions/18078914/notify-an-android-service-when-a-bound-client-disconnects
     *
     * Our existing native disconnect handling might be sufficient.
     */
}
