// Copyright 2026, DisplayXR contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  #1277 P2(a) — the weave satellite's overlay ownership lease.
 *
 * The Android weave satellite presents into a full-panel
 * `TYPE_APPLICATION_OVERLAY`. There is exactly one panel and therefore exactly
 * one such surface worth owning, but ADR-036 D3 gives every client its own
 * satellite *process* (`:dxr0..3`). Two satellites each deciding "the overlay
 * is mine" is undefined behaviour today — the frozen-overlay bug fixed in
 * `91f071770` is the first symptom of that class.
 *
 * This header is the lease's API surface. It is deliberately split in two:
 *
 * - @ref u_overlay_lease_select is the **whole policy**, as a pure function of
 *   facts. It has no state, no IPC and no platform dependency, so it is
 *   host-testable exactly like @ref u_sandbox_route_prop_selects — see
 *   `tests/tests_aux_overlay_lease.cpp`. Android policy decisions that need a
 *   device to be checked are decisions that never get checked.
 * - @ref u_overlay_lease_acquire / @ref u_overlay_lease_release / @ref
 *   u_overlay_lease_held are the **mechanism**, behind a swappable backend.
 *
 * **With no backend installed — the default, and what ships today — every
 * acquire is granted.** One client per satellite process, one satellite
 * running: that is bit-for-bit the shipped single-client behaviour, which is
 * why this file can land ahead of the arbiter it exists for. The backend that
 * makes it a real lease is the main-process slot broker
 * (`SlotBrokerService`, #1053): it is the only object every satellite already
 * talks to, and the only party that can see a satellite's peers. That backend
 * does not exist yet — the broker is Java-only and has no native binding and no
 * callback path (`ISlotBroker.aidl` has three transactions and no listener), so
 * wiring it is new plumbing, tracked as step A3 of the P2 plan in
 * `docs/roadmap/android-weave-satellite.md`.
 *
 * Not thread-safe, by design: every satellite lease decision is already made
 * under the per-client weave mutex (`multi_compositor::weave.mutex`), and
 * adding a second lock here would only invite a new ordering to get wrong.
 *
 * @ingroup aux_util
 */

#pragma once

#include "xrt/xrt_compiler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Slot value meaning "the main-process service", i.e. the client that did not
 * get one of the four `:dxrN` satellite slots (ADR-036 D3). It is a legitimate
 * candidate; that path has simply never been exercised with the satellite.
 *
 * @ingroup aux_util
 */
#define U_OVERLAY_LEASE_SLOT_MAIN (-1)

/*!
 * The number of distinct slots this module can track: the four `:dxrN` slots
 * plus @ref U_OVERLAY_LEASE_SLOT_MAIN.
 *
 * @ingroup aux_util
 */
#define U_OVERLAY_LEASE_MAX_SLOTS 5

/*!
 * Why @ref u_overlay_lease_select picked what it picked. Logged on every
 * transition — the design requires the loser's degradation (it keeps weaving
 * in its own surface, i.e. shows a double image inside a scaled container) to
 * be a stated outcome and never a silent one.
 *
 * @ingroup aux_util
 */
enum u_overlay_lease_reason
{
	//! Nobody wants the overlay.
	U_OVERLAY_LEASE_REASON_NONE = 0,
	//! Exactly one candidate; nothing to arbitrate. Today's single-client case.
	U_OVERLAY_LEASE_REASON_ONLY_CANDIDATE,
	//! Won because its container is scaled and a rival's is not.
	U_OVERLAY_LEASE_REASON_SCALED,
	//! Several equally-entitled candidates; focus broke the tie.
	U_OVERLAY_LEASE_REASON_FOCUS,
	//! Nothing distinguished the candidates, so the current holder kept it.
	U_OVERLAY_LEASE_REASON_INCUMBENT,
	//! Nothing distinguished them and there was no incumbent: lowest slot, for
	//! determinism only. Every process must reach the same answer.
	U_OVERLAY_LEASE_REASON_LOWEST_SLOT,
};

/*!
 * Human-readable form of @ref u_overlay_lease_reason. Never NULL.
 *
 * @ingroup aux_util
 */
const char *
u_overlay_lease_reason_string(enum u_overlay_lease_reason reason);

/*!
 * One client, as the arbiter sees it.
 *
 * @ingroup aux_util
 */
struct u_overlay_lease_candidate
{
	//! ADR-036 satellite slot, or @ref U_OVERLAY_LEASE_SLOT_MAIN.
	int32_t slot;

	/*!
	 * This client is weaving and could use the overlay at all. A client that
	 * is not weaving is not a candidate — it must never take the panel's one
	 * overlay away from a client that is.
	 */
	bool wants_overlay;

	/*!
	 * The P1 hybrid-bounds tell says this client's container is scaled (its
	 * reported window rect exceeds the panel while its origin lies inside).
	 * This is the *only* thing the overlay fixes: an unscaled window weaves
	 * correctly in its own surface and gains nothing by taking it.
	 */
	bool container_scaled;

	/*!
	 * This client has user focus. **All-false is a legitimate steady state**,
	 * not an error: there is no focus authority on Android today, and the
	 * feed that will supply one (the a11y window watcher) is off by default
	 * and dies on an `am force-stop`. The policy below degrades to
	 * "the incumbent keeps it" rather than thrashing when focus is unknown.
	 */
	bool focused;

	//! This client holds the lease right now. Ties go to it, to stop thrash.
	bool incumbent;
};

/*!
 * The overlay arbitration policy, complete, as a pure function.
 *
 * Rules, in order:
 *
 * 1. Only candidates with `wants_overlay` are considered.
 * 2. No candidates ⇒ nobody holds the overlay (`-1`).
 * 3. Exactly one candidate ⇒ that one, unconditionally. This is what keeps the
 *    shipped single-client behaviour bit-for-bit: a lone fullscreen client
 *    still gets the overlay even though it is not scaled.
 * 4. Two or more, and at least one is scaled ⇒ the unscaled ones drop out. The
 *    mixed case therefore answers itself: the scaled window takes the overlay,
 *    the unscaled window keeps weaving in its own surface, and **both are
 *    correct at the same time**.
 * 5. Among the remaining equals: focus wins; else the incumbent keeps it; else
 *    the lowest slot, purely so that every process computes the same answer.
 *
 * @param candidates Array of candidates; may be NULL when @p count is 0.
 * @param count      Number of entries in @p candidates.
 * @param out_reason Optional; why the winner won.
 * @return Index into @p candidates, or `-1` for "nobody holds the overlay".
 *
 * @ingroup aux_util
 */
int
u_overlay_lease_select(const struct u_overlay_lease_candidate *candidates,
                       size_t count,
                       enum u_overlay_lease_reason *out_reason);

/*!
 * The mechanism behind @ref u_overlay_lease_acquire and friends.
 *
 * @ingroup aux_util
 */
struct u_overlay_lease_backend
{
	//! Opaque, passed back to every call.
	void *data;

	/*!
	 * Ask for the overlay. Returns true if this slot now holds it.
	 * Must not block the weave: an implementation that crosses a process
	 * boundary (the slot broker does) may only do so on a *transition*, never
	 * per frame, and never with the weave mutex held.
	 */
	bool (*acquire)(void *data, int32_t slot, const struct u_overlay_lease_candidate *self);

	/*!
	 * Give the overlay back. **Must not return until the overlay has been
	 * cleared and that clear has been presented** — otherwise the outgoing
	 * client's last woven frame is left painted over the incoming one, which
	 * is the `91f071770` bug replayed across clients instead of across time.
	 */
	void (*release)(void *data, int32_t slot);
};

/*!
 * Install (or, with NULL, remove) the backend. Process-global, because the
 * resource it arbitrates is panel-global and each satellite process serves one
 * client. Call once during service start-up, before any client weaves.
 *
 * @ingroup aux_util
 */
void
u_overlay_lease_set_backend(const struct u_overlay_lease_backend *backend);

/*!
 * Acquire the overlay for @p slot.
 *
 * With no backend installed this always grants — the documented default, and
 * exactly today's behaviour for the one-client-per-process topology.
 *
 * @ingroup aux_util
 */
bool
u_overlay_lease_acquire(int32_t slot, const struct u_overlay_lease_candidate *self);

/*!
 * Release the overlay held by @p slot. Idempotent. Synchronous: see the
 * backend contract above.
 *
 * @ingroup aux_util
 */
void
u_overlay_lease_release(int32_t slot);

/*!
 * Does @p slot hold the overlay? Cheap enough for a per-frame gate: with no
 * backend it is a table read, and a real backend must keep this local too
 * (push the revocation, never poll the broker from the weave).
 *
 * @ingroup aux_util
 */
bool
u_overlay_lease_held(int32_t slot);

#ifdef __cplusplus
}
#endif
