// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Presence-gated hand-role arbitration (implementation).
 * @ingroup target_common
 */

#include "target_input_arbiter.h"
#include "target_input_plugin_loader.h"

#include "xrt/xrt_device.h"
#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_system.h"

#include "os/os_threading.h"
#include "os/os_time.h"

#include "util/u_logging.h"

#include <stddef.h>
#include <string.h>


/*!
 * How often the provider is actually asked for its presence. `get_roles`
 * is on the `xrSyncActions` path — per client, per frame, and over IPC —
 * so even a mutex-read provider answer is worth rate-limiting. Presence
 * is level-triggered, so the only cost of the delay is up to this long
 * before an unplug moves the roles to qwerty.
 */
#define ARBITER_PRESENCE_POLL_NS (250LL * 1000 * 1000)

/*!
 * Largest number of candidate pairs we arbitrate between: one per
 * registered provider plus the qwerty floor.
 */
#define ARBITER_MAX_CANDIDATES 17

/*!
 * One competitor for the hand roles. Qwerty is a candidate like any
 * other, distinguished only by a NULL @ref t_input_candidate::iface
 * (nothing to ask — the keyboard is always there) and the lowest
 * possible priority.
 */
struct t_input_candidate
{
	const struct xrt_input_plugin_iface *iface;
	struct xrt_input_plugin_instance *inst;

	//! Ascending = wins. Qwerty uses UINT32_MAX.
	uint32_t priority;

	struct xrt_device *left;
	struct xrt_device *right;

	//! Indices into xsysd->xdevs, resolved at install. -1 = absent.
	int32_t left_index;
	int32_t right_index;

	//! Cached presence verdict for this candidate.
	bool present;
	bool have_verdict;
	int64_t verdict_at_ns;
};

struct t_input_arbiter
{
	//! The system we arbitrate for; NULL until install.
	struct xrt_system_devices *xsysd;

	//! The get_roles we displaced, for any other xsysd.
	xrt_result_t (*fallback_get_roles)(struct xrt_system_devices *xsysd, struct xrt_system_roles *out_roles);

	//! Candidates in priority order: providers first, qwerty last.
	struct t_input_candidate candidates[ARBITER_MAX_CANDIDATES];
	int candidate_count;

	//! What get_roles hands out; generation_id bumps on every flip.
	struct xrt_system_roles roles;
};

static struct t_input_arbiter g_arb = {0};

/*!
 * Guards @ref g_arb. Kept OUT of the struct so @ref t_input_arbiter_reset
 * can wipe the state wholesale without ever destroying a lock a concurrent
 * `get_roles` might be holding. Initialised once, never destroyed — the
 * arbiter lives for the process, like the plug-in loader it sits next to.
 */
static struct os_mutex g_arb_mutex;
static bool g_arb_mutex_ready = false;


/*
 *
 * Helpers.
 *
 */

static int32_t
index_of(struct xrt_system_devices *xsysd, struct xrt_device *xdev)
{
	if (xdev == NULL) {
		return -1;
	}
	for (size_t i = 0; i < xsysd->xdev_count; i++) {
		if (xsysd->xdevs[i] == xdev) {
			return (int32_t)i;
		}
	}
	return -1;
}

static enum xrt_device_name
name_of(struct xrt_device *xdev)
{
	return xdev != NULL ? xdev->name : XRT_DEVICE_INVALID;
}

/*!
 * Ask one candidate whether its hardware is there. Uncached — see
 * @ref candidate_is_present_locked for the throttled entry point.
 */
static bool
query_candidate_presence(const struct t_input_candidate *cand)
{
	if (cand->left == NULL && cand->right == NULL) {
		return false; // Supplied no hand devices at all.
	}
	if (cand->iface == NULL) {
		return true; // Qwerty: the keyboard is always there.
	}
	if (!XRT_INPUT_PLUGIN_IFACE_HAS(cand->iface, get_presence)) {
		// Provider predates the presence slot: assume present for as
		// long as it is loaded, exactly as ADR-034 Phase 1 did.
		return true;
	}

	return cand->iface->get_presence(cand->inst) == XRT_INPUT_PROVIDER_PRESENCE_PRESENT;
}

static const char *
candidate_name(const struct t_input_candidate *cand)
{
	if (cand == NULL) {
		return "none";
	}
	if (cand->iface == NULL) {
		return "qwerty";
	}
	return cand->iface->id != NULL ? cand->iface->id : "provider";
}

//! Caller holds the mutex (or is the single-threaded builder path).
static bool
candidate_is_present_locked(struct t_input_candidate *cand)
{
	int64_t now = os_monotonic_get_ns();

	if (cand->have_verdict && (now - cand->verdict_at_ns) < ARBITER_PRESENCE_POLL_NS) {
		return cand->present;
	}

	bool present = query_candidate_presence(cand);
	if (cand->have_verdict && present != cand->present) {
		U_LOG_W("input arbiter: '%s' hardware %s.", candidate_name(cand), present ? "present again" : "gone");
	}
	cand->present = present;
	cand->have_verdict = true;
	cand->verdict_at_ns = now;
	return present;
}

/*!
 * First present candidate, in priority order, that supplies the hand
 * selected by @p want_left. Returns NULL when nothing does.
 *
 * Resolved per hand on purpose: a provider that supplies only one
 * controller should leave the other hand to the next-ranked candidate,
 * not drag both down with it.
 */
static struct t_input_candidate *
pick_for_hand_locked(bool want_left)
{
	for (int i = 0; i < g_arb.candidate_count; i++) {
		struct t_input_candidate *cand = &g_arb.candidates[i];
		int32_t index = want_left ? cand->left_index : cand->right_index;
		if (index < 0) {
			continue;
		}
		if (candidate_is_present_locked(cand)) {
			return cand;
		}
	}
	return NULL;
}

//! Caller holds the mutex (or is the single-threaded builder path).
static bool
provider_holds_roles_locked(void)
{
	for (int i = 0; i < g_arb.candidate_count; i++) {
		struct t_input_candidate *cand = &g_arb.candidates[i];
		if (cand->iface == NULL) {
			continue; // The qwerty floor is not a provider.
		}
		if (candidate_is_present_locked(cand)) {
			return true;
		}
	}
	return false;
}

/*!
 * Recompute @ref t_input_arbiter::roles for the current verdict, bumping
 * `generation_id` only when the assignment actually changes. Caller holds
 * the mutex.
 */
static void
refresh_roles_locked(void)
{
	struct t_input_candidate *left_cand = pick_for_hand_locked(true);
	struct t_input_candidate *right_cand = pick_for_hand_locked(false);

	int32_t left = left_cand != NULL ? left_cand->left_index : -1;
	int32_t right = right_cand != NULL ? right_cand->right_index : -1;

	if (g_arb.roles.generation_id != 0 && g_arb.roles.left == left && g_arb.roles.right == right) {
		return; // Nothing moved.
	}

	struct xrt_device *left_xdev =
	    (left >= 0 && (size_t)left < g_arb.xsysd->xdev_count) ? g_arb.xsysd->xdevs[left] : NULL;
	struct xrt_device *right_xdev =
	    (right >= 0 && (size_t)right < g_arb.xsysd->xdev_count) ? g_arb.xsysd->xdevs[right] : NULL;

	uint64_t generation = g_arb.roles.generation_id + 1;
	g_arb.roles = (struct xrt_system_roles)XRT_SYSTEM_ROLES_INIT;
	g_arb.roles.generation_id = generation;
	g_arb.roles.left = left;
	g_arb.roles.right = right;
	g_arb.roles.left_profile = name_of(left_xdev);
	g_arb.roles.right_profile = name_of(right_xdev);

	U_LOG_W("input arbiter: hand roles -> left='%s' (%s) right='%s' (%s), generation %u.",
	        left_xdev != NULL ? left_xdev->str : "<none>", candidate_name(left_cand),
	        right_xdev != NULL ? right_xdev->str : "<none>", candidate_name(right_cand), (unsigned)generation);
}

static xrt_result_t
arbiter_get_roles(struct xrt_system_devices *xsysd, struct xrt_system_roles *out_roles)
{
	if (xsysd != g_arb.xsysd) {
		// Not ours (a second system, or teardown raced us) — hand
		// back to whatever we displaced.
		if (g_arb.fallback_get_roles != NULL) {
			return g_arb.fallback_get_roles(xsysd, out_roles);
		}
		*out_roles = (struct xrt_system_roles)XRT_SYSTEM_ROLES_INIT;
		return XRT_SUCCESS;
	}

	os_mutex_lock(&g_arb_mutex);
	refresh_roles_locked();
	*out_roles = g_arb.roles;
	os_mutex_unlock(&g_arb_mutex);

	return XRT_SUCCESS;
}


/*
 *
 * 'Exported' functions.
 *
 */

void
t_input_arbiter_reset(void)
{
	if (!g_arb_mutex_ready) {
		g_arb_mutex_ready = os_mutex_init(&g_arb_mutex) == 0;
		if (!g_arb_mutex_ready) {
			U_LOG_E("input arbiter: mutex init failed — hand roles stay static.");
			return;
		}
	}

	os_mutex_lock(&g_arb_mutex);
	memset(&g_arb, 0, sizeof(g_arb));
	g_arb.roles = (struct xrt_system_roles)XRT_SYSTEM_ROLES_INIT;
	os_mutex_unlock(&g_arb_mutex);
}

/*!
 * Append a candidate keeping the array sorted by priority ascending.
 * Builder-path only (single-threaded), like the note_* callers.
 */
static void
note_candidate(const struct xrt_input_plugin_iface *iface,
               struct xrt_input_plugin_instance *inst,
               uint32_t priority,
               struct xrt_device *left,
               struct xrt_device *right)
{
	if (g_arb.candidate_count >= ARBITER_MAX_CANDIDATES) {
		U_LOG_W("input arbiter: candidate table full — dropping '%s'.",
		        iface != NULL && iface->id != NULL ? iface->id : "?");
		return;
	}

	// Insertion sort: find the first slot with a strictly higher
	// priority value, shift the tail up. Ties keep insertion order.
	int pos = g_arb.candidate_count;
	while (pos > 0 && g_arb.candidates[pos - 1].priority > priority) {
		g_arb.candidates[pos] = g_arb.candidates[pos - 1];
		pos--;
	}

	struct t_input_candidate *cand = &g_arb.candidates[pos];
	memset(cand, 0, sizeof(*cand));
	cand->iface = iface;
	cand->inst = inst;
	cand->priority = priority;
	cand->left = left;
	cand->right = right;
	cand->left_index = -1;
	cand->right_index = -1;
	g_arb.candidate_count++;
}

void
t_input_arbiter_note_provider_pair(const struct xrt_input_plugin_iface *iface,
                                   struct xrt_input_plugin_instance *inst,
                                   uint32_t probe_order,
                                   struct xrt_device *left,
                                   struct xrt_device *right)
{
	if (left == NULL && right == NULL) {
		return; // Nothing to arbitrate with.
	}
	note_candidate(iface, inst, probe_order, left, right);
}

void
t_input_arbiter_note_qwerty_pair(struct xrt_device *left, struct xrt_device *right)
{
	if (left == NULL && right == NULL) {
		return;
	}
	// UINT32_MAX: the floor — every provider outranks the keyboard.
	note_candidate(NULL, NULL, 0xFFFFFFFFu, left, right);
}

bool
t_input_arbiter_provider_holds_roles(void)
{
	if (!g_arb_mutex_ready) {
		// Mutex init failed (t_input_arbiter_reset warned): degrade to
		// the mutex-free walk — the builder path is single-threaded.
		return provider_holds_roles_locked();
	}

	os_mutex_lock(&g_arb_mutex);
	bool holds = provider_holds_roles_locked();
	os_mutex_unlock(&g_arb_mutex);
	return holds;
}

void
t_input_arbiter_install(struct xrt_system_devices *xsysd)
{
	if (xsysd == NULL || !g_arb_mutex_ready) {
		return;
	}

	if (g_arb.candidate_count < 2) {
		// Zero or one candidate: nothing to arbitrate between; the
		// static roles the builder already assigned are the right
		// (and only) answer.
		U_LOG_I("input arbiter: not installed — %s.", g_arb.candidate_count == 0
		                                                  ? "no hand-role candidates at all"
		                                                  : "single candidate owns the hand roles");
		return;
	}

	os_mutex_lock(&g_arb_mutex);

	g_arb.xsysd = xsysd;
	g_arb.fallback_get_roles = xsysd->get_roles;
	for (int i = 0; i < g_arb.candidate_count; i++) {
		struct t_input_candidate *cand = &g_arb.candidates[i];
		cand->left_index = index_of(xsysd, cand->left);
		cand->right_index = index_of(xsysd, cand->right);
		// Force a fresh verdict: the builder's own query may be older
		// than the provider's startup grace.
		cand->have_verdict = false;
	}

	refresh_roles_locked();

	xsysd->get_roles = arbiter_get_roles;

	os_mutex_unlock(&g_arb_mutex);

	for (int i = 0; i < g_arb.candidate_count; i++) {
		const struct t_input_candidate *cand = &g_arb.candidates[i];
		U_LOG_W("input arbiter: candidate [%d] '%s' priority=%u pair (%d,%d).", i, candidate_name(cand),
		        (unsigned)cand->priority, cand->left_index, cand->right_index);
	}
	U_LOG_W("input arbiter: installed — %d candidates; roles re-resolve on every xrSyncActions.",
	        g_arb.candidate_count);
}

void
t_input_arbiter_get_status(bool *out_installed, bool *out_provider_holds_roles)
{
	if (out_installed != NULL) {
		*out_installed = g_arb.xsysd != NULL;
	}
	if (out_provider_holds_roles != NULL) {
		*out_provider_holds_roles = t_input_arbiter_provider_holds_roles();
	}
}
