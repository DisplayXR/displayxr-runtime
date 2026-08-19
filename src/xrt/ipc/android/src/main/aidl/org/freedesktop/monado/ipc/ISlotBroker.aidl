// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Satellite compositor-slot broker (ADR-036 D3, runtime#1031).
 * @ingroup ipc_android
 */

package org.freedesktop.monado.ipc;

/**
 * Assigns each OpenXR client one of the runtime APK's pre-declared satellite
 * compositor processes (`MonadoServiceSlot0..N-1`, `android:process=":dxrN"`).
 *
 * Hosted by the runtime's MAIN process, which owns no compositor and no vendor
 * core — binding this interface must never start a runtime server, which is why
 * it is a separate binder from IMonado (whose stub starts one in its
 * constructor).
 *
 * The client binds the broker, acquires a slot, and then binds THAT slot by
 * explicit ComponentName. It keeps the broker binding for as long as it holds
 * the slot: that is what pins the assignment (the broker links to the caller's
 * token and frees the slot when the client process dies) and what keeps the
 * broker process itself alive while any DisplayXR app is running.
 */
interface ISlotBroker {
    /**
     * Claim a satellite compositor slot.
     *
     * Policy, in order:
     *   1. the package already owns a slot  -> that slot (a relaunch rebinds to
     *      its still-alive satellite instead of stranding it);
     *   2. `preferredSlot` is in range and free -> that slot;
     *   3. the lowest free slot;
     *   4. none free -> -1, and the caller falls back to the single
     *      main-process MonadoService (the legacy one-window path).
     *
     * @param clientPackage advisory client package name; the broker prefers the
     *        name it can resolve from the calling uid and only falls back to
     *        this one, so a client cannot squat another package's slot.
     * @param clientPid     the client's pid, for logging only.
     * @param preferredSlot a pin from `debug.dxr.slot` or the client manifest's
     *        `com.displayxr.satellite_slot` meta-data, or -1 for "no preference".
     * @param clientToken   a binder the client keeps alive for as long as it
     *        uses the slot; the broker links to its death.
     * @return the slot index, or -1 if none is available.
     */
    int acquireSlot(String clientPackage, int clientPid, int preferredSlot, IBinder clientToken);

    /** Give a slot back. Idempotent; a token that owns nothing is ignored. */
    void releaseSlot(int slot, IBinder clientToken);

    /** How many satellite slots this runtime build declares. */
    int getSlotCount();
}
