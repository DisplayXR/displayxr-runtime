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

	/*!
	 * Hand-tracking devices this candidate supplies, same slot layout
	 * as `xrt_system_roles::hand_tracking`. Often the same devices as
	 * @ref left / @ref right (ultraleap). All NULL for qwerty.
	 */
	struct xrt_device *ht_devices[4];

	//! Indices for @ref ht_devices, resolved at install. -1 = absent.
	int32_t ht_indices[4];

	//! Cached presence verdict for this candidate.
	bool present;
	bool have_verdict;
	int64_t verdict_at_ns;
};

//! Slot order for t_input_candidate::ht_devices / ::ht_indices.
enum
{
	ARB_HT_UNOBSTRUCTED_LEFT = 0,
	ARB_HT_UNOBSTRUCTED_RIGHT = 1,
	ARB_HT_CONFORMING_LEFT = 2,
	ARB_HT_CONFORMING_RIGHT = 3,
};

struct t_input_arbiter
{
	//! The system we arbitrate for; NULL until install.
	struct xrt_system_devices *xsysd;

	//! The get_roles we displaced, for any other xsysd.
	xrt_result_t (*fallback_get_roles)(struct xrt_system_devices *xsysd, struct xrt_system_roles *out_roles);

	//! #958: the destroy we displaced, so we can stop the presence poll thread
	//! BEFORE the providers it dereferences are freed.
	void (*fallback_destroy)(struct xrt_system_devices *xsysd);

	//! Candidates in priority order: providers first, qwerty last.
	struct t_input_candidate candidates[ARBITER_MAX_CANDIDATES];
	int candidate_count;

	//! Index of the most recently noted candidate (insertion moves it),
	//! for attaching hand-tracking devices. -1 = none.
	int last_noted;

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

/*!
 * #958: background presence poll. get_presence() must never run on a client
 * thread (it did, under g_arb_mutex on every xrSyncActions — a blocking provider
 * would freeze every client). This thread polls all candidates off any client
 * thread and caches the verdict; candidate_is_present_locked() is now a pure
 * cache read. Kept OUT of g_arb (like g_arb_mutex) so t_input_arbiter_reset's
 * memset never corrupts the helper's own mutex/cond.
 */
static struct os_thread_helper g_arb_poll;
static bool g_arb_poll_ready = false;


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
	bool has_any = cand->left != NULL || cand->right != NULL;
	for (int i = 0; !has_any && i < 4; i++) {
		has_any = cand->ht_devices[i] != NULL;
	}
	if (!has_any) {
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

//! Caller holds g_arb_mutex. #958: a PURE read of the cache the poll thread
//! fills — no provider code runs here (and never on a client thread). Not-yet-
//! verified reads as absent (matches PRESENCE_UNKNOWN).
static bool
candidate_is_present_locked(struct t_input_candidate *cand)
{
	return cand->have_verdict ? cand->present : false;
}

//! #958: write one candidate's fresh verdict into the cache under g_arb_mutex.
//! Called from the poll thread (query done OFF-lock by the caller) and from the
//! synchronous install seed. Logs the level-triggered edge (once per change).
static void
candidate_store_verdict_locked(struct t_input_candidate *cand, bool present)
{
	if (cand->have_verdict && present != cand->present) {
		U_LOG_W("input arbiter: '%s' hardware %s.", candidate_name(cand), present ? "present again" : "gone");
	}
	cand->present = present;
	cand->have_verdict = true;
	cand->verdict_at_ns = os_monotonic_get_ns();
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

//! Same walk for one hand-tracking slot; -1 when no present carrier.
static int32_t
pick_ht_slot_locked(int slot)
{
	for (int i = 0; i < g_arb.candidate_count; i++) {
		struct t_input_candidate *cand = &g_arb.candidates[i];
		if (cand->ht_indices[slot] < 0) {
			continue;
		}
		if (candidate_is_present_locked(cand)) {
			return cand->ht_indices[slot];
		}
	}
	return -1;
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
 * The interaction profile to REPORT for @p winner's device on one hand.
 *
 * Profile stability: real-world WebXR content (three.js
 * `XRControllerModelFactory`) breaks outright on interaction-profile
 * churn — it re-adds a stale cached controller model on the second
 * re-add of a profile and the throw kills the page's render loop. So
 * when the qwerty floor wins a hand on a box that HAS ranked providers,
 * report the top provider's profile instead of qwerty's own — qwerty
 * carries binding-profile remaps for the common controller profiles
 * (simple/touch/index/vive), so the bindings still resolve. Combined
 * with the change-only event rule in oxr, a provider↔qwerty flip then
 * emits no XrEventDataInteractionProfileChanged at all: apps see the
 * pose source move, and nothing else.
 *
 * Falls back to the device's own name when there is no provider to
 * mirror or qwerty cannot emulate its profile. Provider-less boxes are
 * bit-identical to before. Caller holds the mutex.
 */
static enum xrt_device_name
profile_to_report_locked(const struct t_input_candidate *winner, struct xrt_device *xdev, bool want_left)
{
	if (winner == NULL || xdev == NULL) {
		return XRT_DEVICE_INVALID;
	}
	if (winner->iface != NULL) {
		return xdev->name; // A real provider reports itself.
	}

	// The floor won: find the top-ranked provider candidate's device
	// for this hand (either hand as a fallback — the profile is a
	// device-class statement, not a handedness one).
	for (int i = 0; i < g_arb.candidate_count; i++) {
		const struct t_input_candidate *cand = &g_arb.candidates[i];
		if (cand->iface == NULL) {
			continue;
		}
		struct xrt_device *prov = want_left ? cand->left : cand->right;
		if (prov == NULL) {
			prov = want_left ? cand->right : cand->left;
		}
		if (prov == NULL) {
			continue;
		}

		// Only masquerade if the floor device can actually serve the
		// provider's profile through a binding-profile remap.
		for (size_t k = 0; k < xdev->binding_profile_count; k++) {
			if (xdev->binding_profiles[k].name == prov->name) {
				return prov->name;
			}
		}
		break; // Top provider found but not emulatable — be honest.
	}

	return xdev->name;
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

	int32_t ht_ul = pick_ht_slot_locked(ARB_HT_UNOBSTRUCTED_LEFT);
	int32_t ht_ur = pick_ht_slot_locked(ARB_HT_UNOBSTRUCTED_RIGHT);
	int32_t ht_cl = pick_ht_slot_locked(ARB_HT_CONFORMING_LEFT);
	int32_t ht_cr = pick_ht_slot_locked(ARB_HT_CONFORMING_RIGHT);

	if (g_arb.roles.generation_id != 0 && g_arb.roles.left == left && g_arb.roles.right == right &&
	    g_arb.roles.hand_tracking.unobstructed.left == ht_ul &&
	    g_arb.roles.hand_tracking.unobstructed.right == ht_ur &&
	    g_arb.roles.hand_tracking.conforming.left == ht_cl && g_arb.roles.hand_tracking.conforming.right == ht_cr) {
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
	g_arb.roles.left_profile = profile_to_report_locked(left_cand, left_xdev, true);
	g_arb.roles.right_profile = profile_to_report_locked(right_cand, right_xdev, false);
	g_arb.roles.hand_tracking.unobstructed.left = ht_ul;
	g_arb.roles.hand_tracking.unobstructed.right = ht_ur;
	g_arb.roles.hand_tracking.conforming.left = ht_cl;
	g_arb.roles.hand_tracking.conforming.right = ht_cr;

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

/*!
 * #958: presence poll thread. Queries every candidate off any client thread
 * (get_presence is never held under g_arb_mutex — only the scalar cache write
 * is), so a slow/blocking provider stalls this dedicated thread, not a client's
 * xrSyncActions. Candidate array + iface/inst pointers are stable between
 * install and the next reset (which stops this thread first), so reading them
 * off-lock is safe.
 */
static void *
arbiter_presence_poll_thread(void *ptr)
{
	(void)ptr;
	while (os_thread_helper_is_running(&g_arb_poll)) {
		int n = g_arb.candidate_count;
		for (int i = 0; i < n; i++) {
			struct t_input_candidate *cand = &g_arb.candidates[i];
			bool present = query_candidate_presence(cand); // OFF-lock provider call
			os_mutex_lock(&g_arb_mutex);
			candidate_store_verdict_locked(cand, present);
			os_mutex_unlock(&g_arb_mutex);
		}
		// Sleep the poll interval in small chunks so signal_stop is prompt.
		for (int64_t slept = 0; slept < ARBITER_PRESENCE_POLL_NS && os_thread_helper_is_running(&g_arb_poll);
		     slept += 50LL * 1000 * 1000) {
			os_nanosleep(50LL * 1000 * 1000);
		}
	}
	return NULL;
}

/*!
 * #958: displaced xsysd->destroy. Stop the poll thread (which dereferences the
 * candidates' iface/inst) BEFORE the providers are freed, then chain.
 */
static void
arbiter_destroy(struct xrt_system_devices *xsysd)
{
	if (g_arb_poll_ready) {
		os_thread_helper_stop_and_wait(&g_arb_poll);
	}
	if (g_arb.fallback_destroy != NULL) {
		g_arb.fallback_destroy(xsysd);
	}
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
	if (!g_arb_poll_ready) {
		g_arb_poll_ready = os_thread_helper_init(&g_arb_poll) == 0;
	}

	// #958: stop any poll thread from a previous build BEFORE wiping g_arb — it
	// reads g_arb.candidates. A fresh install restarts it.
	if (g_arb_poll_ready) {
		os_thread_helper_stop_and_wait(&g_arb_poll);
	}

	os_mutex_lock(&g_arb_mutex);
	memset(&g_arb, 0, sizeof(g_arb));
	g_arb.last_noted = -1;
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
	for (int i = 0; i < 4; i++) {
		cand->ht_indices[i] = -1;
	}
	g_arb.candidate_count++;
	g_arb.last_noted = pos;
}

void
t_input_arbiter_note_provider_pair(const struct xrt_input_plugin_iface *iface,
                                   struct xrt_input_plugin_instance *inst,
                                   uint32_t probe_order,
                                   struct xrt_device *left,
                                   struct xrt_device *right)
{
	// A NULL/NULL pair is still noted: a provider may carry only
	// hand-tracking devices (see note_provider_hand_tracking), and the
	// candidate is the anchor those attach to.
	note_candidate(iface, inst, probe_order, left, right);
}

void
t_input_arbiter_note_provider_hand_tracking(struct xrt_device *unobstructed_left,
                                            struct xrt_device *unobstructed_right,
                                            struct xrt_device *conforming_left,
                                            struct xrt_device *conforming_right)
{
	// Attach to the most recently noted candidate — the builder calls
	// this right after note_provider_pair for the same provider. The
	// insertion sort keeps ties stable, but the newest candidate is not
	// necessarily last, so find it by having empty HT slots AND being
	// the highest insertion... simplest robust rule: the builder call
	// order guarantees the newest candidate is the ONLY one whose HT
	// slots are all still empty among this provider's — track it
	// explicitly instead.
	if (g_arb.last_noted < 0 || g_arb.last_noted >= g_arb.candidate_count) {
		return;
	}
	struct t_input_candidate *cand = &g_arb.candidates[g_arb.last_noted];
	cand->ht_devices[ARB_HT_UNOBSTRUCTED_LEFT] = unobstructed_left;
	cand->ht_devices[ARB_HT_UNOBSTRUCTED_RIGHT] = unobstructed_right;
	cand->ht_devices[ARB_HT_CONFORMING_LEFT] = conforming_left;
	cand->ht_devices[ARB_HT_CONFORMING_RIGHT] = conforming_right;
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
	g_arb.fallback_destroy = xsysd->destroy; // #958
	for (int i = 0; i < g_arb.candidate_count; i++) {
		struct t_input_candidate *cand = &g_arb.candidates[i];
		cand->left_index = index_of(xsysd, cand->left);
		cand->right_index = index_of(xsysd, cand->right);
		for (int k = 0; k < 4; k++) {
			cand->ht_indices[k] = index_of(xsysd, cand->ht_devices[k]);
		}
		// #958: seed the cache synchronously (single-threaded here) so the
		// refresh below and the first client xrSyncActions / cli selftest get a
		// valid verdict before the poll thread's first tick. The builder's own
		// query may be older than the provider's startup grace, so re-query now.
		cand->have_verdict = false;
		candidate_store_verdict_locked(cand, query_candidate_presence(cand));
	}

	refresh_roles_locked();

	xsysd->get_roles = arbiter_get_roles;
	xsysd->destroy = arbiter_destroy; // #958: stop the poll thread before providers free

	os_mutex_unlock(&g_arb_mutex);

	// #958: start the presence poll now that the candidate array is resolved.
	if (g_arb_poll_ready) {
		if (os_thread_helper_start(&g_arb_poll, arbiter_presence_poll_thread, NULL) != 0) {
			U_LOG_W("input arbiter: presence poll thread failed to start — presence will not refresh.");
		}
	}

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
