// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Presence-gated hand- and rig-role arbitration (implementation).
 * @ingroup target_common
 */

#include "target_input_arbiter.h"
#include "target_input_plugin_loader.h"
#include "target_input_pref.h"

#include "xrt/xrt_device.h"
#include "xrt/xrt_input_plugin.h"
#include "xrt/xrt_input_selection.h"
#include "xrt/xrt_system.h"

#include "os/os_threading.h"
#include "os/os_time.h"

#include "util/u_logging.h"

#include <stddef.h>
#include <stdio.h>
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
 * ADR-034 Amendment 6: the poll thread asks for engagement only while some
 * client resolved roles (a `get_roles`) this recently — a provider may treat
 * the calls as demand and keep its sensor alive, so an idle service with no
 * client must stop making them.
 */
#define ARBITER_ENGAGE_DEMAND_NS (1000LL * 1000 * 1000)

//! Effective engagement of a provider without the slot: continuously engaged while present.
#define ARBITER_ENGAGED_ALWAYS INT64_MAX

/*!
 * Largest number of candidate pairs we arbitrate between: one per
 * registered provider plus the qwerty floor.
 */
#define ARBITER_MAX_CANDIDATES 17

/*!
 * ADR-034 Amendment 5: clients with their own resolution — every IPC
 * connection (IPC_MAX_CLIENTS = 32) plus the implicit in-process client, with
 * slack. A client that finds the table full gets the system-wide roles.
 */
#define ARBITER_MAX_CLIENTS 40

//! Display name of the emulated-controller floor in an enumeration (runtime-owned, vendor-neutral).
#define ARBITER_FLOOR_DISPLAY_NAME "Emulated controllers (keyboard and mouse)"

/*!
 * One competitor for the hand roles. Qwerty is a candidate like any
 * other, distinguished only by a NULL @ref t_input_candidate::iface
 * (nothing to ask — the keyboard is always there) and the lowest
 * possible priority.
 *
 * N7: that candidate is OPTIONAL. With the `EmulatedControllers` gate at
 * 0 the builder notes no pair, so no candidate has a NULL iface, and every
 * walk below simply runs out of candidates — the hands land on -1. Every
 * `iface == NULL` test in this file is therefore "is this the floor", never
 * "there is always a floor".
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

	/*!
	 * The single @ref XRT_DEVICE_TYPE_NAVIGATION device this candidate
	 * supplies, or NULL. Qwerty never has one: the runtime's own fly
	 * camera is the rig floor and is expressed as `roles.rig == -1`, not
	 * as a candidate.
	 */
	struct xrt_device *nav;

	//! Index for @ref nav, resolved at install. -1 = absent.
	int32_t nav_index;

	/*!
	 * The single @ref XRT_DEVICE_TYPE_GAMEPAD device this candidate
	 * supplies, or NULL. Qwerty never has one; the floor is -1.
	 */
	struct xrt_device *gamepad;

	//! Index for @ref gamepad, resolved at install. -1 = absent.
	int32_t gamepad_index;

	/*!
	 * ADR-034 Am.5: the REGISTRATION id (registry subkey / manifest id) — what
	 * a pin or a request names; resolved at install from the loader ("" =
	 * fall back to `iface->id`; the floor is XRT_INPUT_PROVIDER_ID_EMULATED).
	 */
	char row_id[XRT_INPUT_PROVIDER_ID_SIZE];

	//! Cached presence verdict for this candidate.
	bool present;
	bool have_verdict;
	int64_t verdict_at_ns;

	/*!
	 * #2051: withdrawn by policy (`Enabled` = 0 written while the service
	 * runs). Treated by every walk exactly like an absent verdict; the
	 * presence poll keeps running so re-enabling is instant.
	 */
	bool withdrawn;

	/*!
	 * ADR-034 Amendment 6: the last deliberate use per hand (0 = left, 1 =
	 * right) on the os_monotonic_get_ns clock, 0 = never — the provider's
	 * `get_engaged_ns`, or the floor's qwerty input stamp. Written by the
	 * poll thread under the mutex. Unused for a provider without the slot
	 * (@ref ARBITER_ENGAGED_ALWAYS).
	 */
	int64_t engaged_ns[2];

	//! When the current present streak began (an engagement older than this does not count).
	int64_t present_since_ns;
};

//! Slot order for t_input_candidate::ht_devices / ::ht_indices.
enum
{
	ARB_HT_UNOBSTRUCTED_LEFT = 0,
	ARB_HT_UNOBSTRUCTED_RIGHT = 1,
	ARB_HT_CONFORMING_LEFT = 2,
	ARB_HT_CONFORMING_RIGHT = 3,
};

/*!
 * ADR-034 Amendment 5: one client's preference overlay and resolved roles.
 * Lives under g_arb_mutex.
 */
struct t_input_client
{
	bool used;
	uint64_t key; //!< xrt_input_selection_client::key (0 = in-process)
	char exe[XRT_INPUT_EXE_BASENAME_SIZE];

	//! The user's per-app pin for @ref exe ("" = none), cached; re-read on a policy change.
	char pin[XRT_INPUT_PROVIDER_ID_SIZE];

	//! Non-persisted session requests, per family ("" = none).
	char request[XRT_INPUT_ROLE_FAMILY_COUNT][XRT_INPUT_PROVIDER_ID_SIZE];

	//! The effective preference per family at the last resolve, and where it came from.
	char pref_id[XRT_INPUT_ROLE_FAMILY_COUNT][XRT_INPUT_PROVIDER_ID_SIZE];
	enum target_input_pref_source pref_src[XRT_INPUT_ROLE_FAMILY_COUNT];

	//! The resolved roles; generation_id is this client's own, monotonic.
	struct xrt_system_roles roles;
	bool have_roles;

	//! g_arb.epoch at the last resolve; with @ref dirty, the only recompute triggers.
	uint64_t seen_epoch;
	bool dirty;

	//! The first overlay-driven change of this client logged at WARN; later ones at INFO.
	bool warned;
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

	/*!
	 * Candidates in NOTE order (the builder's insertion sort puts them in
	 * priority order at build time). #2051: the array is never reordered
	 * after install — the presence poll thread reads `candidates[i]` off
	 * the lock — so a re-rank rewrites @ref order instead.
	 */
	struct t_input_candidate candidates[ARBITER_MAX_CANDIDATES];
	int candidate_count;

	//! The walk order: indices into @ref candidates, priority ascending
	//! (ties keep note order). Rebuilt by rebuild_order_locked.
	int order[ARBITER_MAX_CANDIDATES];

	//! Index of the most recently noted candidate (insertion moves it),
	//! for attaching hand-tracking devices. -1 = none.
	int last_noted;

	//! What get_roles hands out; generation_id bumps on every flip.
	struct xrt_system_roles roles;

	//! N7: a qwerty pair was noted for this build (a floor candidate exists).
	bool floor_noted;

	/*!
	 * ADR-034 Am.5: bumped on every change a client's resolution depends on —
	 * a presence edge, a re-rank, a floor withdrawal, install. A client
	 * re-resolves only when this moved (or its own overlay did).
	 */
	uint64_t epoch;

	//! Cached class policy (refreshed by t_input_arbiter_policy_changed, off the hot path).
	bool policy_lock;
	char user_default[XRT_INPUT_PROVIDER_ID_SIZE];

	/*!
	 * ADR-034 Amendment 6: `ControllerSwitching` = use (true, the default):
	 * the controller roles follow the most recently engaged present
	 * candidate. False (`presence`): the pre-Amendment-6 rank walk.
	 */
	bool switching_use;

	//! The emulated floor's engagement read (the qwerty builder installs it); NULL = never engaged.
	t_input_arbiter_engaged_fn floor_engaged_fn;

	//! Last `get_roles` on behalf of a client (os_monotonic_get_ns): the engagement-demand gate.
	int64_t last_roles_read_ns;

	//! Per hand, the candidate (index into @ref candidates) engagement put first at the last poll; -1 = none.
	int engage_leader[2];

	//! The first engagement-driven move of the system-wide roles logged at WARN; later ones at INFO.
	bool engage_warned;

	struct t_input_client clients[ARBITER_MAX_CLIENTS];

	//! Moves when any client's resolved roles change or a client comes / goes
	//! (xrt_input_selection::get_generation).
	uint64_t selection_generation;

	/*!
	 * Distinct clients refused an entry (table full), for `input list --json`
	 * `clients_dropped`. Distinct by key over the last @ref dropped_keys, so a
	 * refused client's per-frame get_roles counts once, not per frame.
	 */
	uint32_t clients_dropped;
	uint64_t dropped_keys[8];
	uint32_t dropped_next;
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

//! ADR-034 Am.5: the policy reads (the loader's store). Outside g_arb so a reset keeps them.
static struct t_input_arbiter_policy g_policy;

//! ADR-034 Am.5: the selection object published as xsysd->input_selection. Static, filled at install.
static struct xrt_input_selection g_arb_selection;


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

//! Does this candidate supply anything the HAND roles can use?
static bool
candidate_supplies_hands(const struct t_input_candidate *cand)
{
	bool has_any = cand->left != NULL || cand->right != NULL;
	for (int i = 0; !has_any && i < 4; i++) {
		has_any = cand->ht_devices[i] != NULL;
	}
	return has_any;
}

/*!
 * Ask one candidate whether its hardware is there. Uncached — see
 * @ref candidate_is_present_locked for the throttled entry point.
 */
static bool
query_candidate_presence(const struct t_input_candidate *cand)
{
	if (!candidate_supplies_hands(cand) && cand->nav == NULL && cand->gamepad == NULL) {
		return false; // Supplied no arbitrable device at all.
	}
	if (cand->iface == NULL) {
		// Qwerty: the keyboard is always there. (N7: a floor that was
		// withdrawn by the gate is handled by the cache read, like a
		// withdrawn provider — presence stays true, admission does not.)
		return true;
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
		return "none"; // no candidate won: the N7 "no hand role" floor
	}
	if (cand->iface == NULL) {
		return "qwerty";
	}
	return cand->iface->id != NULL ? cand->iface->id : "provider";
}

//! Caller holds g_arb_mutex. #958: a PURE read of the cache the poll thread
//! fills — no provider code runs here (and never on a client thread). Not-yet-
//! verified reads as absent (matches PRESENCE_UNKNOWN). #2051: a candidate
//! withdrawn by policy reads as absent too.
static bool
candidate_is_present_locked(struct t_input_candidate *cand)
{
	if (cand->withdrawn) {
		return false;
	}
	return cand->have_verdict ? cand->present : false;
}

//! The @p k'th candidate in walk (priority) order. Caller holds the mutex
//! (or is the single-threaded builder path).
static struct t_input_candidate *
ranked_locked(int k)
{
	return &g_arb.candidates[g_arb.order[k]];
}

/*!
 * Rebuild @ref t_input_arbiter::order: priority ascending, ties by note
 * order (a stable insertion sort over at most 17 entries). Caller holds the
 * mutex or is the builder path.
 */
static void
rebuild_order_locked(void)
{
	const int n = g_arb.candidate_count;
	for (int i = 0; i < n; i++) {
		int pos = i;
		while (pos > 0 && g_arb.candidates[g_arb.order[pos - 1]].priority > g_arb.candidates[i].priority) {
			g_arb.order[pos] = g_arb.order[pos - 1];
			pos--;
		}
		g_arb.order[pos] = i;
	}
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
	if (!cand->have_verdict || present != cand->present) {
		g_arb.epoch++; // ADR-034 Am.5: every client's resolution may have moved
		if (present) {
			// Amendment 6: a new present streak — earlier engagement no longer counts.
			cand->present_since_ns = os_monotonic_get_ns();
		}
	}
	cand->present = present;
	cand->have_verdict = true;
	cand->verdict_at_ns = os_monotonic_get_ns();
}

/*!
 * ADR-034 Amendment 6: @p cand's engagement for one hand as the walk compares
 * it — higher wins, ties keep rank order. Three tiers:
 *
 * - A provider WITHOUT the slot is engaged continuously while present
 *   (@ref ARBITER_ENGAGED_ALWAYS), so it keeps the role exactly as before the
 *   amendment.
 * - The emulated floor is the DEFAULT holder: its last input aimed at that
 *   controller, or 1 when the keyboard was never touched — so it holds the
 *   hand over a provider that has not been engaged, from generation 1 on.
 * - A provider WITH the slot counts only once it has been engaged since its
 *   latest return to PRESENT (a provider coming back does not reclaim on an
 *   old pinch); until then 0, the dormant tier — it holds the hand only when
 *   it is the only eligible candidate (no floor: EmulatedControllers = 0),
 *   by rank among other dormant ones.
 *
 * Caller holds the mutex.
 */
static int64_t
effective_engaged_locked(const struct t_input_candidate *cand, bool want_left)
{
	if (cand->iface != NULL && !XRT_INPUT_PLUGIN_IFACE_HAS(cand->iface, get_engaged_ns)) {
		return ARBITER_ENGAGED_ALWAYS;
	}
	const int64_t at = cand->engaged_ns[want_left ? 0 : 1];
	const int64_t counted = at > cand->present_since_ns ? at : 0;
	if (cand->iface == NULL) {
		return counted > 1 ? counted : 1; // the floor: never touched = the default holder
	}
	return counted;
}

/*!
 * The present candidate that supplies the hand selected by @p want_left.
 * Returns NULL when nothing does.
 *
 * @p by_engagement false: the first in priority order (presence-gated rank,
 * ADR-034 Amendment 3). True (Amendment 6): the highest
 * @ref effective_engaged_locked — the most recently engaged candidate, the
 * keyboard floor by default, a never-engaged slot provider only when nothing
 * else is eligible — ties keeping priority order.
 *
 * Resolved per hand on purpose: a provider that supplies only one
 * controller should leave the other hand to the next-ranked candidate,
 * not drag both down with it.
 */
static struct t_input_candidate *
pick_for_hand_locked(bool want_left, bool by_engagement)
{
	struct t_input_candidate *best = NULL;
	int64_t best_engaged = 0;
	for (int i = 0; i < g_arb.candidate_count; i++) {
		struct t_input_candidate *cand = ranked_locked(i);
		int32_t index = want_left ? cand->left_index : cand->right_index;
		if (index < 0) {
			continue;
		}
		if (!candidate_is_present_locked(cand)) {
			continue;
		}
		if (!by_engagement) {
			return cand;
		}
		const int64_t engaged = effective_engaged_locked(cand, want_left);
		if (best == NULL || engaged > best_engaged) {
			best = cand;
			best_engaged = engaged;
		}
	}
	return best;
}

/*!
 * ADR-034 Amendment 6: does the system-wide walk follow engagement? Only with
 * `ControllerSwitching` = use AND no `PreferredPlugin` naming a controller
 * candidate — the user default is a hard choice, like a pin. Caller holds
 * the mutex.
 */
static bool
candidate_claims_controllers_by_id_locked(const char *id);

static bool
system_follows_engagement_locked(void)
{
	return g_arb.switching_use && !candidate_claims_controllers_by_id_locked(g_arb.user_default);
}

/*!
 * Same walk for the rig (navigation) role: the highest-priority present
 * candidate that supplies a navigation device. -1 when none does — which
 * is not "no rig" but "the runtime's own fly camera holds it", the state
 * every box without a navigating provider is in (ADR-034 Amendment 4).
 */
static int32_t
pick_rig_locked(void)
{
	for (int i = 0; i < g_arb.candidate_count; i++) {
		struct t_input_candidate *cand = ranked_locked(i);
		if (cand->nav_index < 0) {
			continue;
		}
		if (candidate_is_present_locked(cand)) {
			return cand->nav_index;
		}
	}
	return -1;
}

/*!
 * Same walk for the gamepad role: the highest-priority present candidate
 * that supplies a @ref XRT_DEVICE_TYPE_GAMEPAD device. -1 when none does —
 * there is no qwerty gamepad, so the floor is "no gamepad", which is what
 * every box without a pad provider (or with its pad unplugged) reports.
 */
static int32_t
pick_gamepad_locked(void)
{
	for (int i = 0; i < g_arb.candidate_count; i++) {
		struct t_input_candidate *cand = ranked_locked(i);
		if (cand->gamepad_index < 0) {
			continue;
		}
		if (candidate_is_present_locked(cand)) {
			return cand->gamepad_index;
		}
	}
	return -1;
}

//! Same walk for one hand-tracking slot; -1 when no present carrier.
static int32_t
pick_ht_slot_locked(int slot)
{
	for (int i = 0; i < g_arb.candidate_count; i++) {
		struct t_input_candidate *cand = ranked_locked(i);
		if (cand->ht_indices[slot] < 0) {
			continue;
		}
		if (candidate_is_present_locked(cand)) {
			return cand->ht_indices[slot];
		}
	}
	return -1;
}

/*!
 * Caller holds the mutex (or is the single-threaded builder path). Is the
 * candidate that supplies the navigation device at @p nav_index present?
 *
 * The rig counterpart of @ref provider_holds_roles_locked: `roles.rig == -1`
 * alone cannot say whether a navigating provider is unplugged (correct) or
 * present and failing to take the role (a fault), and this is what tells the
 * two apart (#1465).
 */
static bool
nav_candidate_present_locked(int32_t nav_index)
{
	if (nav_index < 0) {
		return false; // Not an index: -1 IS the fly-camera floor.
	}
	for (int i = 0; i < g_arb.candidate_count; i++) {
		struct t_input_candidate *cand = &g_arb.candidates[i];
		if (cand->nav_index != nav_index) {
			continue;
		}
		return candidate_is_present_locked(cand);
	}
	return false; // No candidate owns it at all.
}

//! Caller holds the mutex (or is the single-threaded builder path).
static bool
provider_holds_roles_locked(void)
{
	for (int i = 0; i < g_arb.candidate_count; i++) {
		struct t_input_candidate *cand = ranked_locked(i);
		if (cand->iface == NULL) {
			continue; // The qwerty floor is not a provider (and may be absent, N7).
		}
		if (!candidate_supplies_hands(cand)) {
			continue; // Navigation-only: present, but not for the HANDS.
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
		// Nobody won the hand (N7: no floor, every provider absent or
		// withdrawn): no device, no profile — the OpenXR layer reports
		// XR_NULL_PATH, and its change-only rule emits ONE
		// XrEventDataInteractionProfileChanged per flip, not a storm.
		return XRT_DEVICE_INVALID;
	}
	if (winner->iface != NULL) {
		return xdev->name; // A real provider reports itself.
	}

	// The floor won: find the top-ranked provider candidate's device
	// for this hand (either hand as a fallback — the profile is a
	// device-class statement, not a handedness one). #2051: a withdrawn
	// provider is not "there" for an app any more than an unplugged one.
	for (int i = 0; i < g_arb.candidate_count; i++) {
		const struct t_input_candidate *cand = ranked_locked(i);
		if (cand->iface == NULL || cand->withdrawn) {
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
	const bool by_engagement = system_follows_engagement_locked();
	struct t_input_candidate *left_cand = pick_for_hand_locked(true, by_engagement);
	struct t_input_candidate *right_cand = pick_for_hand_locked(false, by_engagement);

	int32_t left = left_cand != NULL ? left_cand->left_index : -1;
	int32_t right = right_cand != NULL ? right_cand->right_index : -1;

	int32_t ht_ul = pick_ht_slot_locked(ARB_HT_UNOBSTRUCTED_LEFT);
	int32_t ht_ur = pick_ht_slot_locked(ARB_HT_UNOBSTRUCTED_RIGHT);
	int32_t ht_cl = pick_ht_slot_locked(ARB_HT_CONFORMING_LEFT);
	int32_t ht_cr = pick_ht_slot_locked(ARB_HT_CONFORMING_RIGHT);

	int32_t rig = pick_rig_locked();
	int32_t gamepad = pick_gamepad_locked();

	if (g_arb.roles.generation_id != 0 && g_arb.roles.left == left && g_arb.roles.right == right &&
	    g_arb.roles.hand_tracking.unobstructed.left == ht_ul &&
	    g_arb.roles.hand_tracking.unobstructed.right == ht_ur &&
	    g_arb.roles.hand_tracking.conforming.left == ht_cl && g_arb.roles.hand_tracking.conforming.right == ht_cr &&
	    g_arb.roles.rig == rig && g_arb.roles.gamepad == gamepad) {
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
	g_arb.roles.rig = rig;

	// The gamepad reports its own profile (a provider device, never a
	// masquerading floor): -1 / INVALID when no present candidate has one.
	struct xrt_device *gamepad_xdev =
	    (gamepad >= 0 && (size_t)gamepad < g_arb.xsysd->xdev_count) ? g_arb.xsysd->xdevs[gamepad] : NULL;
	g_arb.roles.gamepad = gamepad;
	g_arb.roles.gamepad_profile = name_of(gamepad_xdev);

	struct xrt_device *rig_xdev =
	    (rig >= 0 && (size_t)rig < g_arb.xsysd->xdev_count) ? g_arb.xsysd->xdevs[rig] : NULL;

	// Amendment 6: a move engagement made (the rank walk would have picked
	// differently) is a user switching devices — human rate, but it can be
	// frequent, so WARN the first one and INFO after. Everything else (presence,
	// policy, install) stays WARN as before.
	const bool engage_move = by_engagement && (left_cand != pick_for_hand_locked(true, false) ||
	                                           right_cand != pick_for_hand_locked(false, false));
	const bool loud = !engage_move || !g_arb.engage_warned;
	if (engage_move) {
		g_arb.engage_warned = true;
	}
	U_LOG_IFL(loud ? U_LOGGING_WARN : U_LOGGING_INFO, u_log_get_global_level(),
	          "input arbiter: hand roles -> left='%s' (%s) right='%s' (%s), rig -> '%s', gamepad -> '%s', "
	          "generation %u%s.",
	          left_xdev != NULL ? left_xdev->str : "<none>", candidate_name(left_cand),
	          right_xdev != NULL ? right_xdev->str : "<none>", candidate_name(right_cand),
	          rig_xdev != NULL ? rig_xdev->str : "<runtime fly camera>",
	          gamepad_xdev != NULL ? gamepad_xdev->str : "<none>", (unsigned)generation,
	          engage_move ? " (controllers follow the engaged device, ControllerSwitching = use)" : "");
}

/*
 *
 * Per-client resolution (ADR-034 Amendment 5, #2181).
 *
 */

//! The id a pin / request names this candidate by.
static const char *
candidate_id(const struct t_input_candidate *cand)
{
	if (cand->iface == NULL) {
		return XRT_INPUT_PROVIDER_ID_EMULATED;
	}
	if (cand->row_id[0] != '\0') {
		return cand->row_id;
	}
	return cand->iface->id != NULL ? cand->iface->id : "";
}

//! The candidate a preference id names (registration id first, then the binary's own id), or NULL.
static struct t_input_candidate *
candidate_by_id_locked(const char *id)
{
	if (id == NULL || id[0] == '\0') {
		return NULL;
	}
	for (int i = 0; i < g_arb.candidate_count; i++) {
		struct t_input_candidate *cand = &g_arb.candidates[i];
		if (strcmp(candidate_id(cand), id) == 0) {
			return cand;
		}
	}
	for (int i = 0; i < g_arb.candidate_count; i++) {
		struct t_input_candidate *cand = &g_arb.candidates[i];
		if (cand->iface != NULL && cand->iface->id != NULL && strcmp(cand->iface->id, id) == 0) {
			return cand;
		}
	}
	return NULL;
}

//! Does @p cand supply devices of @p family? (Decision 2: a preference applies only where it does.)
static bool
candidate_claims(const struct t_input_candidate *cand, enum xrt_input_role_family family)
{
	if (cand == NULL) {
		return false;
	}
	switch (family) {
	case XRT_INPUT_ROLE_FAMILY_CONTROLLERS: return cand->left_index >= 0 || cand->right_index >= 0;
	case XRT_INPUT_ROLE_FAMILY_HAND_TRACKING:
		for (int k = 0; k < 4; k++) {
			if (cand->ht_indices[k] >= 0) {
				return true;
			}
		}
		return false;
	case XRT_INPUT_ROLE_FAMILY_GAMEPAD: return cand->gamepad_index >= 0;
	default: return false;
	}
}

static bool
candidate_claims_controllers_by_id_locked(const char *id)
{
	return candidate_claims(candidate_by_id_locked(id), XRT_INPUT_ROLE_FAMILY_CONTROLLERS);
}

static uint32_t
candidate_claimable_bits(const struct t_input_candidate *cand)
{
	uint32_t bits = 0;
	for (int f = 0; f < XRT_INPUT_ROLE_FAMILY_COUNT; f++) {
		if (candidate_claims(cand, (enum xrt_input_role_family)f)) {
			bits |= XRT_INPUT_ROLE_FAMILY_BIT(f);
		}
	}
	return bits;
}

static struct t_input_client *
client_find_locked(uint64_t key)
{
	for (int i = 0; i < ARBITER_MAX_CLIENTS; i++) {
		if (g_arb.clients[i].used && g_arb.clients[i].key == key) {
			return &g_arb.clients[i];
		}
	}
	return NULL;
}

/*!
 * The client for @p ref (NULL = the in-process implicit client), creating it
 * on first use. Returns with g_arb_mutex HELD — also when it returns NULL
 * (table full). The store reads a new client needs (its executable, its pin)
 * happen before the lock is taken, never under it.
 */
static struct t_input_client *
client_get_and_lock(const struct xrt_input_selection_client *ref)
{
	const uint64_t key = ref != NULL ? ref->key : 0;

	os_mutex_lock(&g_arb_mutex);
	struct t_input_client *c = client_find_locked(key);
	if (c != NULL) {
		return c;
	}
	os_mutex_unlock(&g_arb_mutex);

	char exe[XRT_INPUT_EXE_BASENAME_SIZE] = {0};
	if (ref != NULL && ref->exe_basename[0] != '\0') {
		snprintf(exe, sizeof(exe), "%s", ref->exe_basename);
	} else if (key == 0 && g_policy.self_exe_basename != NULL) {
		(void)g_policy.self_exe_basename(exe, sizeof(exe));
	}
	char pin[XRT_INPUT_PROVIDER_ID_SIZE] = {0};
	if (exe[0] != '\0' && g_policy.get_app_pin != NULL) {
		(void)g_policy.get_app_pin(exe, pin, sizeof(pin));
	}

	os_mutex_lock(&g_arb_mutex);
	c = client_find_locked(key); // another thread may have attached it meanwhile
	if (c != NULL) {
		return c;
	}
	for (int i = 0; i < ARBITER_MAX_CLIENTS; i++) {
		if (!g_arb.clients[i].used) {
			c = &g_arb.clients[i];
			memset(c, 0, sizeof(*c));
			c->used = true;
			c->key = key;
			snprintf(c->exe, sizeof(c->exe), "%s", exe);
			snprintf(c->pin, sizeof(c->pin), "%s", pin);
			c->dirty = true;
			if (pin[0] != '\0') {
				U_LOG_W("input arbiter: client %llu ('%s') attached with per-app pin '%s'.",
				        (unsigned long long)key, exe, pin);
			}
			return c;
		}
	}
	// Table full. Count each refused client once (keys are monotonic over IPC;
	// the small ring keeps a refused client's per-frame calls from recounting).
	bool seen = false;
	for (int i = 0; i < 8 && !seen; i++) {
		seen = g_arb.dropped_keys[i] == key + 1; // +1: key 0 is a valid key, 0 is "empty"
	}
	if (!seen) {
		g_arb.dropped_keys[g_arb.dropped_next++ % 8] = key + 1;
		g_arb.clients_dropped++;
		if (g_arb.clients_dropped == 1) {
			U_LOG_W(
			    "input arbiter: per-client table full (%d) — client %llu ('%s') gets the system-wide "
			    "roles; its "
			    "per-app pin and requests do not apply (see `input list --json` clients_dropped).",
			    ARBITER_MAX_CLIENTS, (unsigned long long)key, exe);
		} else {
			U_LOG_I("input arbiter: per-client table full — client %llu dropped (%u so far).",
			        (unsigned long long)key, (unsigned)g_arb.clients_dropped);
		}
	}
	return NULL;
}

//! @p hoisted when it supplies @p index_of_hoisted (>= 0) and is present, else NULL.
static struct t_input_candidate *
hoisted_if_present_locked(struct t_input_candidate *hoisted, int32_t index_of_hoisted)
{
	if (hoisted == NULL || index_of_hoisted < 0) {
		return NULL;
	}
	return candidate_is_present_locked(hoisted) ? hoisted : NULL;
}

static bool
roles_equal(const struct xrt_system_roles *a, const struct xrt_system_roles *b)
{
	return a->left == b->left && a->right == b->right && a->gamepad == b->gamepad &&
	       a->left_profile == b->left_profile && a->right_profile == b->right_profile &&
	       a->gamepad_profile == b->gamepad_profile &&
	       a->hand_tracking.unobstructed.left == b->hand_tracking.unobstructed.left &&
	       a->hand_tracking.unobstructed.right == b->hand_tracking.unobstructed.right &&
	       a->hand_tracking.conforming.left == b->hand_tracking.conforming.left &&
	       a->hand_tracking.conforming.right == b->hand_tracking.conforming.right && a->rig == b->rig;
}

static struct xrt_device *
xdev_at_locked(int32_t index)
{
	return (index >= 0 && (size_t)index < g_arb.xsysd->xdev_count) ? g_arb.xsysd->xdevs[index] : NULL;
}

/*!
 * Re-resolve @p c's roles when the arbiter's state or its own overlay moved
 * since the last time; a no-op otherwise (so `get_roles` stays a cache read).
 * Caller holds the mutex; never the presence poll thread.
 */
static void
resolve_client_locked(struct t_input_client *c)
{
	refresh_roles_locked(); // the system-wide walk: the rig, and the generation a new client seeds from
	if (c->have_roles && !c->dirty && c->seen_epoch == g_arb.epoch) {
		return;
	}

	// The preferred candidate per family (decision 3's precedence, decision 2's claim gate).
	struct t_input_candidate *hoist[XRT_INPUT_ROLE_FAMILY_COUNT] = {0};
	bool any_overlay = false;
	for (int f = 0; f < XRT_INPUT_ROLE_FAMILY_COUNT; f++) {
		const enum xrt_input_role_family fam = (enum xrt_input_role_family)f;
		struct target_input_pref_inputs in;
		memset(&in, 0, sizeof(in));
		in.lock = g_arb.policy_lock;
		in.app_pin = c->pin;
		in.app_pin_claims = candidate_claims(candidate_by_id_locked(c->pin), fam);
		in.request = c->request[f];
		in.request_claims = candidate_claims(candidate_by_id_locked(c->request[f]), fam);
		in.user_default = g_arb.user_default;
		in.user_default_claims = candidate_claims(candidate_by_id_locked(g_arb.user_default), fam);
		c->pref_src[f] = target_input_resolve_preference(&in, c->pref_id[f], sizeof(c->pref_id[f]));
		// The user default is already rank 0 of the default walk: only a pin or a
		// request hoists anything.
		if (c->pref_src[f] == TARGET_INPUT_PREF_APP_PIN || c->pref_src[f] == TARGET_INPUT_PREF_REQUEST) {
			hoist[f] = candidate_by_id_locked(c->pref_id[f]);
			any_overlay = any_overlay || hoist[f] != NULL;
		}
	}

	// Amendment 6: the controller roles follow the engaged device only when
	// nothing names a controller candidate for this client — a pin, a request,
	// or the user default (Policy\Lock is folded into the precedence above) —
	// and the class policy allows it. A preference is a hard choice; with one,
	// an absent preferred candidate falls through to the plain rank walk.
	const bool by_engagement =
	    g_arb.switching_use && c->pref_src[XRT_INPUT_ROLE_FAMILY_CONTROLLERS] == TARGET_INPUT_PREF_NONE;
	struct t_input_candidate *hc = hoist[XRT_INPUT_ROLE_FAMILY_CONTROLLERS];
	struct t_input_candidate *left_cand = hoisted_if_present_locked(hc, hc != NULL ? hc->left_index : -1);
	if (left_cand == NULL) {
		left_cand = pick_for_hand_locked(true, by_engagement);
	}
	struct t_input_candidate *right_cand = hoisted_if_present_locked(hc, hc != NULL ? hc->right_index : -1);
	if (right_cand == NULL) {
		right_cand = pick_for_hand_locked(false, by_engagement);
	}
	const int32_t left = left_cand != NULL ? left_cand->left_index : -1;
	const int32_t right = right_cand != NULL ? right_cand->right_index : -1;

	int32_t ht[4];
	struct t_input_candidate *hh = hoist[XRT_INPUT_ROLE_FAMILY_HAND_TRACKING];
	for (int k = 0; k < 4; k++) {
		struct t_input_candidate *w = hoisted_if_present_locked(hh, hh != NULL ? hh->ht_indices[k] : -1);
		ht[k] = w != NULL ? w->ht_indices[k] : pick_ht_slot_locked(k);
	}

	struct t_input_candidate *hg = hoist[XRT_INPUT_ROLE_FAMILY_GAMEPAD];
	struct t_input_candidate *gw = hoisted_if_present_locked(hg, hg != NULL ? hg->gamepad_index : -1);
	const int32_t gamepad = gw != NULL ? gw->gamepad_index : pick_gamepad_locked();

	struct xrt_system_roles next = XRT_SYSTEM_ROLES_INIT;
	next.left = left;
	next.right = right;
	next.left_profile = profile_to_report_locked(left_cand, xdev_at_locked(left), true);
	next.right_profile = profile_to_report_locked(right_cand, xdev_at_locked(right), false);
	next.hand_tracking.unobstructed.left = ht[ARB_HT_UNOBSTRUCTED_LEFT];
	next.hand_tracking.unobstructed.right = ht[ARB_HT_UNOBSTRUCTED_RIGHT];
	next.hand_tracking.conforming.left = ht[ARB_HT_CONFORMING_LEFT];
	next.hand_tracking.conforming.right = ht[ARB_HT_CONFORMING_RIGHT];
	next.gamepad = gamepad;
	next.gamepad_profile = name_of(xdev_at_locked(gamepad));
	next.rig = g_arb.roles.rig; // runtime-wide, never per client

	c->seen_epoch = g_arb.epoch;
	c->dirty = false;
	if (c->have_roles && roles_equal(&next, &c->roles)) {
		return; // nothing moved for THIS client: its generation stays
	}
	// Decision 4: one monotonic generation per client, seeded from the system's.
	const uint64_t seed = g_arb.roles.generation_id > 0 ? g_arb.roles.generation_id : 1;
	next.generation_id = c->have_roles ? c->roles.generation_id + 1 : seed;
	c->roles = next;
	c->have_roles = true;
	g_arb.selection_generation++; // a holder moved for this client (or it just attached): one status bump

	if (any_overlay) {
		// An overlay-driven assignment change: event-driven (a request, a pin, a
		// presence edge), never per frame — WARN the first time per client, INFO
		// after, so a provider whose presence toggles often stays quiet.
		const bool first = !c->warned;
		c->warned = true;
		U_LOG_IFL(first ? U_LOGGING_WARN : U_LOGGING_INFO, u_log_get_global_level(),
		          "input arbiter: client %llu ('%s') roles -> left %d right %d hands %d/%d gamepad %d "
		          "(controllers '%s', hands '%s', gamepad '%s'), generation %llu.",
		          (unsigned long long)c->key, c->exe, left, right, ht[ARB_HT_UNOBSTRUCTED_LEFT],
		          ht[ARB_HT_UNOBSTRUCTED_RIGHT], gamepad, c->pref_id[0][0] ? c->pref_id[0] : "-",
		          c->pref_id[1][0] ? c->pref_id[1] : "-", c->pref_id[2][0] ? c->pref_id[2] : "-",
		          (unsigned long long)next.generation_id);
	}
}

/*!
 * The roles for @p ref, through the per-client overlay. Falls back to the
 * system-wide roles when the client table is full.
 */
static void
client_roles(const struct xrt_input_selection_client *ref, struct xrt_system_roles *out_roles)
{
	struct t_input_client *c = client_get_and_lock(ref); // returns locked
	// Amendment 6: a live role read — the poll thread keeps asking providers
	// for engagement (demand) while these keep coming.
	g_arb.last_roles_read_ns = os_monotonic_get_ns();
	if (c == NULL) {
		refresh_roles_locked();
		*out_roles = g_arb.roles;
	} else {
		resolve_client_locked(c);
		*out_roles = c->roles;
	}
	os_mutex_unlock(&g_arb_mutex);
}

//! The candidate that owns system-device index @p index for @p family, or NULL.
static const struct t_input_candidate *
owner_of_locked(int32_t index, enum xrt_input_role_family family)
{
	if (index < 0) {
		return NULL;
	}
	for (int i = 0; i < g_arb.candidate_count; i++) {
		const struct t_input_candidate *cand = &g_arb.candidates[i];
		switch (family) {
		case XRT_INPUT_ROLE_FAMILY_CONTROLLERS:
			if (cand->left_index == index || cand->right_index == index) {
				return cand;
			}
			break;
		case XRT_INPUT_ROLE_FAMILY_HAND_TRACKING:
			for (int k = 0; k < 4; k++) {
				if (cand->ht_indices[k] == index) {
					return cand;
				}
			}
			break;
		case XRT_INPUT_ROLE_FAMILY_GAMEPAD:
			if (cand->gamepad_index == index) {
				return cand;
			}
			break;
		default: break;
		}
	}
	return NULL;
}

//! Families of @p roles held by @p cand.
static uint32_t
current_bits_locked(const struct t_input_candidate *cand, const struct xrt_system_roles *roles)
{
	uint32_t bits = 0;
	if (owner_of_locked(roles->left, XRT_INPUT_ROLE_FAMILY_CONTROLLERS) == cand ||
	    owner_of_locked(roles->right, XRT_INPUT_ROLE_FAMILY_CONTROLLERS) == cand) {
		bits |= XRT_INPUT_ROLE_FAMILY_BIT(XRT_INPUT_ROLE_FAMILY_CONTROLLERS);
	}
	const int32_t ht[4] = {roles->hand_tracking.unobstructed.left, roles->hand_tracking.unobstructed.right,
	                       roles->hand_tracking.conforming.left, roles->hand_tracking.conforming.right};
	for (int k = 0; k < 4; k++) {
		if (owner_of_locked(ht[k], XRT_INPUT_ROLE_FAMILY_HAND_TRACKING) == cand) {
			bits |= XRT_INPUT_ROLE_FAMILY_BIT(XRT_INPUT_ROLE_FAMILY_HAND_TRACKING);
			break;
		}
	}
	if (owner_of_locked(roles->gamepad, XRT_INPUT_ROLE_FAMILY_GAMEPAD) == cand) {
		bits |= XRT_INPUT_ROLE_FAMILY_BIT(XRT_INPUT_ROLE_FAMILY_GAMEPAD);
	}
	return bits;
}

//! Holder id of @p family in @p roles ("" = nobody).
static void
holder_of_locked(const struct xrt_system_roles *roles, enum xrt_input_role_family family, char *out, size_t cap)
{
	const struct t_input_candidate *cand = NULL;
	switch (family) {
	case XRT_INPUT_ROLE_FAMILY_CONTROLLERS:
		cand = owner_of_locked(roles->left, family);
		if (cand == NULL) {
			cand = owner_of_locked(roles->right, family);
		}
		break;
	case XRT_INPUT_ROLE_FAMILY_HAND_TRACKING: {
		const int32_t ht[4] = {roles->hand_tracking.unobstructed.left, roles->hand_tracking.unobstructed.right,
		                       roles->hand_tracking.conforming.left, roles->hand_tracking.conforming.right};
		for (int k = 0; k < 4 && cand == NULL; k++) {
			cand = owner_of_locked(ht[k], family);
		}
		break;
	}
	case XRT_INPUT_ROLE_FAMILY_GAMEPAD: cand = owner_of_locked(roles->gamepad, family); break;
	default: break;
	}
	snprintf(out, cap, "%s", cand != NULL ? candidate_id(cand) : "");
}


/*
 *
 * The xrt_input_selection published as xsysd->input_selection.
 *
 */

static xrt_result_t
sel_get_roles(struct xrt_input_selection *xis,
              const struct xrt_input_selection_client *client,
              struct xrt_system_roles *out_roles)
{
	(void)xis;
	if (out_roles == NULL || g_arb.xsysd == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	client_roles(client, out_roles);
	return XRT_SUCCESS;
}

static xrt_result_t
sel_enumerate(struct xrt_input_selection *xis,
              const struct xrt_input_selection_client *client,
              uint32_t start,
              struct xrt_input_provider_page *out_page)
{
	(void)xis;
	if (out_page == NULL || g_arb.xsysd == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	memset(out_page, 0, sizeof(*out_page));
	out_page->start = start;

	struct t_input_client *c = client_get_and_lock(client); // returns locked
	struct xrt_system_roles roles;
	if (c != NULL) {
		resolve_client_locked(c);
		roles = c->roles;
	} else {
		refresh_roles_locked();
		roles = g_arb.roles;
	}
	out_page->generation = roles.generation_id;

	uint32_t listed = 0;
	for (int i = 0; i < g_arb.candidate_count; i++) {
		const struct t_input_candidate *cand = ranked_locked(i);
		const uint32_t claimable = candidate_claimable_bits(cand);
		if (cand->withdrawn || claimable == 0) {
			continue; // disabled by policy, or navigation-only: not a choice for any family
		}
		const uint32_t pos = listed++;
		if (pos < start || out_page->count >= XRT_INPUT_PROVIDER_PAGE_MAX) {
			continue;
		}
		struct xrt_input_provider_info *info = &out_page->items[out_page->count++];
		memset(info, 0, sizeof(*info));
		const char *id = candidate_id(cand);
		snprintf(info->id, sizeof(info->id), "%s", id);
		if (cand->iface == NULL) {
			snprintf(info->display_name, sizeof(info->display_name), "%s", ARBITER_FLOOR_DISPLAY_NAME);
			info->builtin = true;
		} else {
			snprintf(info->display_name, sizeof(info->display_name), "%s",
			         cand->iface->display_name != NULL ? cand->iface->display_name : id);
		}
		info->claimable_families = claimable;
		info->current_families = current_bits_locked(cand, &roles);
		info->present = candidate_is_present_locked((struct t_input_candidate *)cand);
		info->user_default = g_arb.user_default[0] != '\0' && strcmp(g_arb.user_default, id) == 0;
		if (c != NULL) {
			info->app_pinned =
			    !g_arb.policy_lock && c->pin[0] != '\0' && candidate_by_id_locked(c->pin) == cand;
			for (int f = 0; f < XRT_INPUT_ROLE_FAMILY_COUNT; f++) {
				if (c->request[f][0] != '\0' && candidate_by_id_locked(c->request[f]) == cand) {
					info->requested_families |= XRT_INPUT_ROLE_FAMILY_BIT(f);
				}
			}
		}
	}
	out_page->total = listed;
	os_mutex_unlock(&g_arb_mutex);
	return XRT_SUCCESS;
}

static xrt_result_t
sel_set_preference(struct xrt_input_selection *xis,
                   const struct xrt_input_selection_client *client,
                   enum xrt_input_role_family family,
                   const char *provider_id,
                   bool persist)
{
	(void)xis;
	if ((int)family < 0 || family >= XRT_INPUT_ROLE_FAMILY_COUNT || g_arb.xsysd == NULL) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	char id[XRT_INPUT_PROVIDER_ID_SIZE] = {0};
	if (provider_id != NULL) {
		snprintf(id, sizeof(id), "%s", provider_id);
	}

	struct t_input_client *c = client_get_and_lock(client); // returns locked
	if (c == NULL) {
		os_mutex_unlock(&g_arb_mutex);
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	const uint64_t key = c->key;
	char exe[XRT_INPUT_EXE_BASENAME_SIZE];
	snprintf(exe, sizeof(exe), "%s", c->exe);
	if (!persist || id[0] == '\0') {
		// A clear (persisted or not) clears the session request too.
		snprintf(c->request[family], sizeof(c->request[family]), "%s", persist ? "" : id);
		c->dirty = true;
	}
	os_mutex_unlock(&g_arb_mutex);

	if (!persist) {
		U_LOG_W("input arbiter: client %llu ('%s') requests family %d -> '%s'.", (unsigned long long)key, exe,
		        (int)family, id[0] != '\0' ? id : "<default>");
		return XRT_SUCCESS;
	}

	// Persist = the user's per-app pin record (the Dashboard's), written off the lock.
	if (exe[0] == '\0' || g_policy.set_app_pin == NULL || g_policy.set_app_pin(exe, id) != 0) {
		U_LOG_W("input arbiter: client %llu: per-app pin for '%s' could not be written — refused.",
		        (unsigned long long)key, exe[0] != '\0' ? exe : "<unknown executable>");
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	char pin[XRT_INPUT_PROVIDER_ID_SIZE] = {0};
	if (g_policy.get_app_pin != NULL) {
		(void)g_policy.get_app_pin(exe, pin, sizeof(pin)); // re-read: Policy\Lock may hide it
	}
	os_mutex_lock(&g_arb_mutex);
	// Every client of the same executable shares the pin (the record is per app).
	for (int i = 0; i < ARBITER_MAX_CLIENTS; i++) {
		struct t_input_client *o = &g_arb.clients[i];
		if (o->used && strcmp(o->exe, exe) == 0) {
			snprintf(o->pin, sizeof(o->pin), "%s", pin);
			o->dirty = true;
		}
	}
	os_mutex_unlock(&g_arb_mutex);
	U_LOG_W("input arbiter: per-app pin for '%s' %s '%s' (by client %llu).", exe, id[0] ? "set to" : "cleared",
	        id[0] ? id : "", (unsigned long long)key);
	return XRT_SUCCESS;
}

static xrt_result_t
sel_query_holders(struct xrt_input_selection *xis, uint32_t index, struct xrt_input_client_holders *out)
{
	(void)xis;
	if (out == NULL || g_arb.xsysd == NULL || !g_arb_mutex_ready) {
		return XRT_ERROR_FEATURE_NOT_SUPPORTED;
	}
	memset(out, 0, sizeof(*out));
	os_mutex_lock(&g_arb_mutex);
	uint32_t seen = 0;
	for (int i = 0; i < ARBITER_MAX_CLIENTS; i++) {
		struct t_input_client *c = &g_arb.clients[i];
		if (!c->used || seen++ != index) {
			continue;
		}
		resolve_client_locked(c);
		out->key = c->key;
		snprintf(out->exe_basename, sizeof(out->exe_basename), "%s", c->exe);
		for (int f = 0; f < XRT_INPUT_ROLE_FAMILY_COUNT; f++) {
			holder_of_locked(&c->roles, (enum xrt_input_role_family)f, out->holder[f],
			                 sizeof(out->holder[f]));
			const bool overlay =
			    c->pref_src[f] == TARGET_INPUT_PREF_APP_PIN || c->pref_src[f] == TARGET_INPUT_PREF_REQUEST;
			snprintf(out->preferred[f], sizeof(out->preferred[f]), "%s", overlay ? c->pref_id[f] : "");
		}
		out->generation = c->roles.generation_id;
		out->clients_dropped = g_arb.clients_dropped;
		os_mutex_unlock(&g_arb_mutex);
		return XRT_SUCCESS;
	}
	os_mutex_unlock(&g_arb_mutex);
	return XRT_ERROR_FEATURE_NOT_SUPPORTED; // past the end
}

static uint64_t
sel_get_generation(struct xrt_input_selection *xis)
{
	(void)xis;
	if (!g_arb_mutex_ready) {
		return 0;
	}
	os_mutex_lock(&g_arb_mutex);
	const uint64_t g = g_arb.selection_generation;
	os_mutex_unlock(&g_arb_mutex);
	return g;
}

static void
sel_client_detach(struct xrt_input_selection *xis, const struct xrt_input_selection_client *client)
{
	(void)xis;
	if (client == NULL || !g_arb_mutex_ready) {
		return;
	}
	os_mutex_lock(&g_arb_mutex);
	struct t_input_client *c = client_find_locked(client->key);
	if (c != NULL) {
		memset(c, 0, sizeof(*c)); // requests die with the client; the user's pin stays in the store
		g_arb.selection_generation++;
	}
	os_mutex_unlock(&g_arb_mutex);
}

/*!
 * Re-read the cached class policy (lock, user default) — store I/O, so never
 * under the lock. Install and @ref t_input_arbiter_policy_changed call it.
 */
static void
read_class_policy(bool *out_lock, char *out_default, size_t cap, bool *out_switching_use)
{
	*out_lock = g_policy.get_policy_lock != NULL && g_policy.get_policy_lock();
	out_default[0] = '\0';
	if (g_policy.get_user_default != NULL) {
		(void)g_policy.get_user_default(out_default, cap);
	}
	// Amendment 6: absent slot = the default, `use`.
	*out_switching_use = g_policy.get_controller_switching == NULL || g_policy.get_controller_switching();
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

	// ADR-034 Am.5: this process's own view — the implicit client (key 0),
	// keyed by this process's executable. With no preference set, its walk
	// is the default walk and the roles are the system-wide ones.
	//
	// In the SERVICE this makes the service itself a key-0 client keyed by
	// its own executable (`displayxr-service`). That is deliberate and
	// harmless: IPC clients are keyed by the server's client id (never 0),
	// nobody pins the service, and its own role reads keep the default walk.
	// Do not "fix" it by bypassing client_roles: the per-client generation
	// is the one every in-process caller has always seen.
	client_roles(NULL, out_roles);
	return XRT_SUCCESS;
}

/*!
 * Amendment 6: read @p cand's engagement for the hands it supplies, OFF the
 * lock (provider code). False when it has nothing to read: a provider
 * without the slot (continuously engaged — nothing to ask), or a floor with
 * no reader installed.
 */
static bool
query_candidate_engagement(const struct t_input_candidate *cand, t_input_arbiter_engaged_fn floor_fn, int64_t out[2])
{
	if (cand->iface == NULL) {
		if (floor_fn == NULL) {
			return false;
		}
		out[0] = cand->left != NULL ? floor_fn(cand->left) : 0;
		out[1] = cand->right != NULL ? floor_fn(cand->right) : 0;
		return true;
	}
	if (!XRT_INPUT_PLUGIN_IFACE_HAS(cand->iface, get_engaged_ns)) {
		return false;
	}
	out[0] = cand->left != NULL ? cand->iface->get_engaged_ns(cand->inst, XRT_INPUT_PLUGIN_HAND_LEFT) : 0;
	out[1] = cand->right != NULL ? cand->iface->get_engaged_ns(cand->inst, XRT_INPUT_PLUGIN_HAND_RIGHT) : 0;
	return true;
}

/*!
 * Amendment 6: who would engagement put first per hand now? When that moved,
 * bump the epoch so every client re-resolves (and `generation_id` moves where
 * its roles did). A candidate refreshing an engagement it already leads with
 * moves nothing — no per-tick churn while the user keeps typing. Caller holds
 * the mutex; called from the poll thread, which only writes the cache.
 */
static void
note_engage_leaders_locked(void)
{
	for (int h = 0; h < 2; h++) {
		const struct t_input_candidate *lead = pick_for_hand_locked(h == 0, true);
		const int idx = lead != NULL ? (int)(lead - g_arb.candidates) : -1;
		if (idx != g_arb.engage_leader[h]) {
			g_arb.engage_leader[h] = idx;
			g_arb.epoch++;
		}
	}
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

		// Amendment 6: ask for engagement only while a client is resolving
		// roles (a provider may treat the call as demand), and only when the
		// policy uses it.
		os_mutex_lock(&g_arb_mutex);
		const bool want_engagement =
		    g_arb.switching_use && g_arb.last_roles_read_ns != 0 &&
		    os_monotonic_get_ns() - g_arb.last_roles_read_ns < ARBITER_ENGAGE_DEMAND_NS;
		t_input_arbiter_engaged_fn floor_fn = g_arb.floor_engaged_fn;
		os_mutex_unlock(&g_arb_mutex);

		for (int i = 0; i < n; i++) {
			struct t_input_candidate *cand = &g_arb.candidates[i];
			bool present = query_candidate_presence(cand); // OFF-lock provider call
			bool withdrawn = false;
			if (want_engagement && present) {
				os_mutex_lock(&g_arb_mutex);
				withdrawn = cand->withdrawn; // a withdrawn candidate is not asked (no demand)
				os_mutex_unlock(&g_arb_mutex);
			}
			int64_t engaged[2] = {0, 0};
			const bool read_engaged = want_engagement && present && !withdrawn &&
			                          query_candidate_engagement(cand, floor_fn, engaged); // OFF-lock
			os_mutex_lock(&g_arb_mutex);
			candidate_store_verdict_locked(cand, present);
			if (read_engaged) {
				cand->engaged_ns[0] = engaged[0];
				cand->engaged_ns[1] = engaged[1];
			}
			os_mutex_unlock(&g_arb_mutex);
		}
		if (want_engagement) {
			os_mutex_lock(&g_arb_mutex);
			note_engage_leaders_locked();
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
	if (xsysd != NULL && xsysd->input_selection == &g_arb_selection) {
		xsysd->input_selection = NULL;
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
	g_arb.engage_leader[0] = -1;
	g_arb.engage_leader[1] = -1;
	g_arb.switching_use = true;
	g_arb.roles = (struct xrt_system_roles)XRT_SYSTEM_ROLES_INIT;
	rebuild_order_locked();
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
	cand->nav_index = -1;
	cand->gamepad_index = -1;
	for (int i = 0; i < 4; i++) {
		cand->ht_indices[i] = -1;
	}
	g_arb.candidate_count++;
	g_arb.last_noted = pos;
	rebuild_order_locked(); // note order == priority order at build time
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
t_input_arbiter_note_provider_navigation(struct xrt_device *const *devs, uint32_t count)
{
	// Attaches to the candidate note_provider_pair just created, exactly
	// like the hand-tracking devices do.
	if (devs == NULL || g_arb.last_noted < 0 || g_arb.last_noted >= g_arb.candidate_count) {
		return;
	}
	struct t_input_candidate *cand = &g_arb.candidates[g_arb.last_noted];

	for (uint32_t i = 0; i < count; i++) {
		struct xrt_device *xdev = devs[i];
		if (xdev == NULL || xdev->device_type != XRT_DEVICE_TYPE_NAVIGATION) {
			continue;
		}
		if (cand->nav != NULL) {
			// One rig per provider by contract — a second one is a
			// provider bug, and silently picking a different device
			// each build would be worse than picking the first.
			U_LOG_W("input arbiter: '%s' supplied more than one navigation device — keeping '%s'.",
			        candidate_name(cand), cand->nav->str);
			break;
		}
		cand->nav = xdev;
	}
}

void
t_input_arbiter_note_provider_gamepad(struct xrt_device *const *devs, uint32_t count)
{
	// Attaches to the candidate note_provider_pair just created, exactly
	// like the navigation device does.
	if (devs == NULL || g_arb.last_noted < 0 || g_arb.last_noted >= g_arb.candidate_count) {
		return;
	}
	struct t_input_candidate *cand = &g_arb.candidates[g_arb.last_noted];

	for (uint32_t i = 0; i < count; i++) {
		struct xrt_device *xdev = devs[i];
		if (xdev == NULL || xdev->device_type != XRT_DEVICE_TYPE_GAMEPAD) {
			continue;
		}
		if (cand->gamepad != NULL) {
			U_LOG_W("input arbiter: '%s' supplied more than one gamepad device — keeping '%s'.",
			        candidate_name(cand), cand->gamepad->str);
			break;
		}
		cand->gamepad = xdev;
	}
}

void
t_input_arbiter_note_qwerty_pair(struct xrt_device *left, struct xrt_device *right)
{
	if (left == NULL && right == NULL) {
		return; // N7: no pair published -> no floor candidate -> hands fall to -1.
	}
	// UINT32_MAX: the floor — every provider outranks the keyboard.
	note_candidate(NULL, NULL, 0xFFFFFFFFu, left, right);
	g_arb.floor_noted = true;
}

void
t_input_arbiter_set_floor_engagement(t_input_arbiter_engaged_fn fn)
{
	if (g_arb_mutex_ready) {
		os_mutex_lock(&g_arb_mutex);
		g_arb.floor_engaged_fn = fn;
		os_mutex_unlock(&g_arb_mutex);
	} else {
		g_arb.floor_engaged_fn = fn;
	}
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

bool
t_input_arbiter_nav_candidate_present(int32_t nav_index)
{
	if (!g_arb_mutex_ready) {
		// Mutex init failed (t_input_arbiter_reset warned): degrade to
		// the mutex-free walk — the builder path is single-threaded.
		return nav_candidate_present_locked(nav_index);
	}

	os_mutex_lock(&g_arb_mutex);
	bool present = nav_candidate_present_locked(nav_index);
	os_mutex_unlock(&g_arb_mutex);
	return present;
}

void
t_input_arbiter_install(struct xrt_system_devices *xsysd)
{
	if (xsysd == NULL || !g_arb_mutex_ready) {
		return;
	}

	// A navigation or gamepad candidate forces the arbiter in (below): the
	// builder has no static seed for `roles.rig`, and the gamepad's static
	// seed must become presence-gated so an unplugged pad really is gone.
	bool have_nav = false;
	for (int i = 0; i < g_arb.candidate_count; i++) {
		have_nav |= g_arb.candidates[i].nav != NULL || g_arb.candidates[i].gamepad != NULL;
	}

	bool have_floor = false;
	for (int i = 0; i < g_arb.candidate_count; i++) {
		have_floor |= g_arb.candidates[i].iface == NULL;
	}

	// Zero candidates, or qwerty alone: nothing to arbitrate between; the
	// static roles the builder already assigned are the right (and only)
	// answer. Two exceptions need the arbiter reachable anyway: a navigation
	// device (the builder has no static seed for `roles.rig`), and — N7 — a
	// lone PROVIDER with no floor under it, whose hands must fall to -1 when
	// its hardware goes (and come back), which only the walk can do.
	const bool lone_floor = g_arb.candidate_count == 1 && have_floor;
	if ((g_arb.candidate_count == 0 || lone_floor) && !have_nav) {
		U_LOG_I("input arbiter: not installed — %s.",
		        g_arb.candidate_count == 0
		            ? (t_input_arbiter_has_floor() ? "no hand-role candidates at all"
		                                           : "no hand-role candidates at all (EmulatedControllers=0: "
		                                             "the hand roles stay unassigned)")
		            : "single candidate (qwerty) owns the hand roles");
		return;
	}

	// ADR-034 Am.5: what pins and requests name — the registration id — and the
	// class policy they resolve under. Store reads, so before the lock.
	for (int i = 0; i < g_arb.candidate_count; i++) {
		struct t_input_candidate *cand = &g_arb.candidates[i];
		const char *row = (cand->iface != NULL && g_policy.row_id_for != NULL)
		                      ? g_policy.row_id_for(cand->iface, cand->inst)
		                      : NULL;
		snprintf(cand->row_id, sizeof(cand->row_id), "%s", row != NULL ? row : "");
	}
	bool policy_lock = false;
	bool switching_use = true;
	char user_default[XRT_INPUT_PROVIDER_ID_SIZE];
	read_class_policy(&policy_lock, user_default, sizeof(user_default), &switching_use);

	os_mutex_lock(&g_arb_mutex);

	g_arb.policy_lock = policy_lock;
	g_arb.switching_use = switching_use;
	snprintf(g_arb.user_default, sizeof(g_arb.user_default), "%s", user_default);
	g_arb.epoch++;

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
		cand->nav_index = index_of(xsysd, cand->nav);
		cand->gamepad_index = index_of(xsysd, cand->gamepad);
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

	// ADR-034 Am.5: per-client selection, for the OpenXR layer (in-process) and
	// the IPC server (on each connection's behalf).
	g_arb_selection.get_roles = sel_get_roles;
	g_arb_selection.enumerate = sel_enumerate;
	g_arb_selection.set_preference = sel_set_preference;
	g_arb_selection.query_holders = sel_query_holders;
	g_arb_selection.client_detach = sel_client_detach;
	g_arb_selection.get_generation = sel_get_generation;
	xsysd->input_selection = &g_arb_selection;

	os_mutex_unlock(&g_arb_mutex);

	// #958: start the presence poll now that the candidate array is resolved.
	if (g_arb_poll_ready) {
		if (os_thread_helper_start(&g_arb_poll, arbiter_presence_poll_thread, NULL) != 0) {
			U_LOG_W("input arbiter: presence poll thread failed to start — presence will not refresh.");
		}
	}

	for (int i = 0; i < g_arb.candidate_count; i++) {
		const struct t_input_candidate *cand = &g_arb.candidates[i];
		U_LOG_W("input arbiter: candidate [%d] '%s' priority=%u pair (%d,%d) nav (%d) gamepad (%d).", i,
		        candidate_name(cand), (unsigned)cand->priority, cand->left_index, cand->right_index,
		        cand->nav_index, cand->gamepad_index);
		// A provider built before the presence slot is ASSUMED present for as
		// long as it is loaded (query_candidate_presence), so with no hardware
		// attached it still outranks qwerty and every hosted / WebXR app sees
		// controllers that never go active. Say so where `displayxr-cli info`
		// and the service log show the roles, or the stale DLL is invisible.
		if (cand->iface != NULL && !XRT_INPUT_PLUGIN_IFACE_HAS(cand->iface, get_presence)) {
			U_LOG_W(
			    "input arbiter: candidate '%s' predates the presence slot (no get_presence) — "
			    "ASSUMED PRESENT while loaded, so it holds the hand roles even with no hardware; "
			    "rebuild or re-register the provider so qwerty can take over when it is absent.",
			    candidate_name(cand));
		}
	}
	U_LOG_W("input arbiter: installed — %d candidates%s; roles re-resolve on every xrSyncActions.",
	        g_arb.candidate_count,
	        have_floor ? "" : " (no qwerty floor: hands fall to -1 when every provider is absent)");
}

bool
t_input_arbiter_has_floor(void)
{
	return g_arb.floor_noted;
}

int
t_input_arbiter_set_floor_withdrawn(bool withdrawn)
{
	if (!g_arb_mutex_ready) {
		return 0;
	}
	os_mutex_lock(&g_arb_mutex);
	if (g_arb.xsysd == NULL) {
		os_mutex_unlock(&g_arb_mutex);
		return 0; // Not installed: the static roles stand until the next build.
	}
	int changed = 0;
	for (int i = 0; i < g_arb.candidate_count; i++) {
		struct t_input_candidate *cand = &g_arb.candidates[i];
		if (cand->iface != NULL || cand->withdrawn == withdrawn) {
			continue;
		}
		U_LOG_W("input arbiter: qwerty floor %s.",
		        withdrawn ? "WITHDRAWN (EmulatedControllers = 0; devices stay resident; no hand-role floor)"
		                  : "re-admitted (EmulatedControllers = 1)");
		cand->withdrawn = withdrawn;
		changed++;
	}
	if (changed > 0) {
		g_arb.epoch++;
		refresh_roles_locked(); // bumps generation_id and logs when the assignment moved
	}
	os_mutex_unlock(&g_arb_mutex);
	return changed;
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

int
t_input_arbiter_rerank(const struct t_input_arbiter_rank *ranks, int count)
{
	if (ranks == NULL || count <= 0 || !g_arb_mutex_ready) {
		return 0;
	}
	os_mutex_lock(&g_arb_mutex);
	if (g_arb.xsysd == NULL) {
		os_mutex_unlock(&g_arb_mutex);
		return 0; // Not installed: nothing arbitrates, nothing to re-rank.
	}
	int changed = 0;
	for (int i = 0; i < g_arb.candidate_count; i++) {
		struct t_input_candidate *cand = &g_arb.candidates[i];
		if (cand->iface == NULL || cand->iface->id == NULL) {
			continue; // qwerty: never ranked by policy (its gate is set_floor_withdrawn, N7)
		}
		for (int k = 0; k < count; k++) {
			if (ranks[k].id == NULL || strcmp(ranks[k].id, cand->iface->id) != 0) {
				continue;
			}
			const bool withdraw = !ranks[k].enabled;
			if (cand->priority != ranks[k].priority || cand->withdrawn != withdraw) {
				U_LOG_W("input arbiter: '%s' priority %u -> %u%s.", candidate_name(cand),
				        (unsigned)cand->priority, (unsigned)ranks[k].priority,
				        withdraw != cand->withdrawn
				            ? (withdraw ? ", WITHDRAWN (Enabled = 0; devices stay resident)"
				                        : ", re-admitted (Enabled = 1)")
				            : "");
				cand->priority = ranks[k].priority;
				cand->withdrawn = withdraw;
				changed++;
			}
			break;
		}
	}
	if (changed > 0) {
		g_arb.epoch++;
		rebuild_order_locked();
		refresh_roles_locked(); // bumps generation_id and logs when the assignment moved
	}
	os_mutex_unlock(&g_arb_mutex);
	return changed;
}

void
t_input_arbiter_set_policy(const struct t_input_arbiter_policy *policy)
{
	if (policy != NULL) {
		g_policy = *policy;
	} else {
		memset(&g_policy, 0, sizeof(g_policy));
	}
}

int
t_input_arbiter_policy_changed(void)
{
	if (!g_arb_mutex_ready) {
		return 0;
	}
	bool policy_lock = false;
	bool switching_use = true;
	char user_default[XRT_INPUT_PROVIDER_ID_SIZE];
	read_class_policy(&policy_lock, user_default, sizeof(user_default), &switching_use);

	// Snapshot who is attached, read their pins off the lock, apply by key.
	struct
	{
		uint64_t key;
		char exe[XRT_INPUT_EXE_BASENAME_SIZE];
		char pin[XRT_INPUT_PROVIDER_ID_SIZE];
	} snap[ARBITER_MAX_CLIENTS];
	int n = 0;
	os_mutex_lock(&g_arb_mutex);
	if (g_arb.xsysd == NULL) {
		os_mutex_unlock(&g_arb_mutex);
		return 0; // not installed: nothing resolves per client
	}
	for (int i = 0; i < ARBITER_MAX_CLIENTS; i++) {
		if (g_arb.clients[i].used) {
			snap[n].key = g_arb.clients[i].key;
			snprintf(snap[n].exe, sizeof(snap[n].exe), "%s", g_arb.clients[i].exe);
			snap[n].pin[0] = '\0';
			n++;
		}
	}
	os_mutex_unlock(&g_arb_mutex);

	for (int i = 0; i < n; i++) {
		if (snap[i].exe[0] != '\0' && g_policy.get_app_pin != NULL) {
			(void)g_policy.get_app_pin(snap[i].exe, snap[i].pin, sizeof(snap[i].pin));
		}
	}

	int changed = 0;
	os_mutex_lock(&g_arb_mutex);
	g_arb.policy_lock = policy_lock;
	snprintf(g_arb.user_default, sizeof(g_arb.user_default), "%s", user_default);
	if (g_arb.switching_use != switching_use) {
		U_LOG_W("input arbiter: ControllerSwitching -> %s.",
		        switching_use ? "use (the controller roles follow the engaged device)"
		                      : "presence (the controller roles follow presence + rank only)");
		g_arb.switching_use = switching_use;
		g_arb.epoch++;
	}
	for (int i = 0; i < n; i++) {
		struct t_input_client *c = client_find_locked(snap[i].key);
		if (c == NULL) {
			continue; // detached meanwhile
		}
		if (strcmp(c->pin, snap[i].pin) != 0) {
			U_LOG_W("input arbiter: client %llu ('%s') per-app pin '%s' -> '%s'.",
			        (unsigned long long)c->key, c->exe, c->pin, snap[i].pin);
			snprintf(c->pin, sizeof(c->pin), "%s", snap[i].pin);
			changed++;
		}
	}
	// The lock or the user default may have moved too: every client re-resolves
	// at its next get_roles (never here, never on the poll thread).
	for (int i = 0; i < ARBITER_MAX_CLIENTS; i++) {
		if (g_arb.clients[i].used) {
			g_arb.clients[i].dirty = true;
		}
	}
	os_mutex_unlock(&g_arb_mutex);
	return changed;
}
