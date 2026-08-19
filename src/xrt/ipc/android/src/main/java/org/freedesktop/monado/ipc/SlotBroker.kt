// Copyright 2026, Leia Inc.
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Assignment policy for the satellite compositor slots.
 * @ingroup ipc_android
 */
package org.freedesktop.monado.ipc

import android.content.Context
import android.os.Binder
import android.os.IBinder
import android.util.Log

/**
 * The satellite compositor-slot broker (ADR-036 D3, #1031).
 *
 * One instance lives in the runtime APK's **main** process and hands each OpenXR client one of the
 * pre-declared `MonadoServiceSlot0..N-1` components. Every satellite then runs the ordinary
 * out-of-process compositor for exactly one client: one `comp_multi`, one `android_globals`
 * surface, one display processor, one vendor core.
 *
 * State is deliberately soft. If the last client goes away the broker process may be reclaimed and
 * the table with it — which is correct, because with no clients every slot is free anyway. While
 * any client holds its broker binding, `BIND_ABOVE_CLIENT` keeps this process at the client's
 * importance, so the table outlives everything that depends on it.
 */
class SlotBroker(context: Context, private val slotCount: Int) : ISlotBroker.Stub() {

    private val packageManager = context.packageManager

    /** One owner per occupied slot. `tokens` is a set because a relaunching client re-acquires. */
    private class Owner(val pkg: String, var pid: Int) {
        val tokens = mutableSetOf<IBinder>()
    }

    private val lock = Any()

    private val slots = arrayOfNulls<Owner>(slotCount)

    override fun getSlotCount(): Int = slotCount

    override fun acquireSlot(
        clientPackage: String?,
        clientPid: Int,
        preferredSlot: Int,
        clientToken: IBinder?,
    ): Int {
        if (clientToken == null) {
            Log.e(TAG, "acquireSlot: no client token")
            return -1
        }
        val pkg = resolveCallerPackage(clientPackage)
        synchronized(lock) {
            // 1. Reuse — a relaunch of the same package rebinds to its own satellite rather
            //    than stranding a live one and burning a second slot.
            var slot = slots.indexOfFirst { it != null && it.pkg == pkg }
            var why = "reuse"
            if (slot < 0) {
                // 2. Honour a pin (debug.dxr.slot / com.displayxr.satellite_slot) if it is free.
                if (preferredSlot in 0 until slotCount && slots[preferredSlot] == null) {
                    slot = preferredSlot
                    why = "pinned"
                } else {
                    // 3. Lowest free slot.
                    slot = slots.indexOfFirst { it == null }
                    why = "lowest-free"
                }
            }
            if (slot < 0) {
                Log.w(
                    TAG,
                    "acquireSlot: all $slotCount slots busy (${occupancyLocked()}) — $pkg " +
                        "falls back to the main-process service",
                )
                return -1
            }
            val owner = slots[slot] ?: Owner(pkg, clientPid).also { slots[slot] = it }
            owner.pid = clientPid
            if (owner.tokens.add(clientToken)) {
                try {
                    clientToken.linkToDeath(DeathHandler(slot, clientToken), 0)
                } catch (e: Exception) {
                    // The client died between binding and this call; treat it as never acquired.
                    owner.tokens.remove(clientToken)
                    if (owner.tokens.isEmpty()) {
                        slots[slot] = null
                    }
                    Log.w(TAG, "acquireSlot: client $pkg died during acquire: $e")
                    return -1
                }
            }
            Log.w(
                TAG,
                "acquireSlot: $pkg pid=$clientPid pref=$preferredSlot -> slot $slot ($why); " +
                    "occupancy ${occupancyLocked()}",
            )
            return slot
        }
    }

    override fun releaseSlot(slot: Int, clientToken: IBinder?) {
        if (clientToken == null || slot < 0 || slot >= slotCount) {
            return
        }
        try {
            clientToken.unlinkToDeath(DeathHandler(slot, clientToken), 0)
        } catch (e: Exception) {
            // Already gone; the death handler did (or is doing) the work.
        }
        dropToken(slot, clientToken, "released")
    }

    /** A client process died — free whatever it held. */
    private inner class DeathHandler(private val slot: Int, private val token: IBinder) :
        IBinder.DeathRecipient {
        override fun binderDied() {
            dropToken(slot, token, "client died")
        }

        // The token identifies the recipient for unlinkToDeath, so two handlers for the same
        // (slot, token) pair must compare equal.
        override fun equals(other: Any?): Boolean =
            other is DeathHandler && other.slot == slot && other.token === token

        override fun hashCode(): Int = slot * 31 + System.identityHashCode(token)
    }

    private fun dropToken(slot: Int, token: IBinder, reason: String) {
        synchronized(lock) {
            val owner = slots[slot] ?: return
            if (!owner.tokens.remove(token)) {
                return
            }
            if (owner.tokens.isEmpty()) {
                slots[slot] = null
                Log.w(
                    TAG,
                    "releaseSlot: slot $slot freed (${owner.pkg}, $reason); " +
                        "occupancy ${occupancyLocked()}",
                )
            }
        }
    }

    /**
     * Identity comes from the calling uid, not from the caller's own claim: the broker is exported,
     * so a client could otherwise name another app's package and be handed its satellite. The
     * claimed name is accepted only when it really belongs to the calling uid (shared-uid packages),
     * which is also what keeps this correct for the runtime's own in-APK clients.
     */
    private fun resolveCallerPackage(claimed: String?): String {
        val uid = Binder.getCallingUid()
        val owned = packageManager.getPackagesForUid(uid)
        if (owned != null && owned.isNotEmpty()) {
            if (claimed != null && owned.contains(claimed)) {
                return claimed
            }
            if (claimed != null) {
                Log.w(TAG, "acquireSlot: uid $uid claimed '$claimed'; using '${owned[0]}'")
            }
            return owned[0]
        }
        return claimed ?: "uid:$uid"
    }

    private fun occupancyLocked(): String =
        slots.mapIndexed { i, o -> "$i=${o?.pkg ?: "-"}" }.joinToString(" ")

    companion object {
        private const val TAG = "dxr-slot-broker"
    }
}
