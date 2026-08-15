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

struct t_input_arbiter
{
	//! The system we arbitrate for; NULL until install.
	struct xrt_system_devices *xsysd;

	//! The get_roles we displaced, for any other xsysd.
	xrt_result_t (*fallback_get_roles)(struct xrt_system_devices *xsysd, struct xrt_system_roles *out_roles);

	struct xrt_device *provider_left;
	struct xrt_device *provider_right;
	struct xrt_device *qwerty_left;
	struct xrt_device *qwerty_right;

	//! Indices into xsysd->xdevs, resolved at install. -1 = absent.
	int32_t provider_left_index;
	int32_t provider_right_index;
	int32_t qwerty_left_index;
	int32_t qwerty_right_index;

	//! What get_roles hands out; generation_id bumps on every flip.
	struct xrt_system_roles roles;

	//! Cached provider verdict + when it was taken.
	bool provider_holds;
	bool have_verdict;
	int64_t verdict_at_ns;
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
 * Ask the active provider whether its hardware is there. Uncached — see
 * @ref t_input_arbiter_provider_holds_roles for the throttled entry point.
 */
static bool
query_provider_presence(void)
{
	if (g_arb.provider_left == NULL && g_arb.provider_right == NULL) {
		return false; // No provider devices at all.
	}

	const struct xrt_input_plugin_iface *iface = target_input_plugin_get_active();
	if (iface == NULL) {
		return false;
	}
	if (!XRT_INPUT_PLUGIN_IFACE_HAS(iface, get_presence)) {
		// Provider predates the presence slot: it holds the roles for
		// as long as it is loaded, exactly as ADR-034 Phase 1 did.
		return true;
	}

	enum xrt_input_provider_presence presence = iface->get_presence(target_input_plugin_get_active_instance());
	return presence == XRT_INPUT_PROVIDER_PRESENCE_PRESENT;
}

//! Caller holds the mutex (or is the single-threaded builder path).
static bool
provider_holds_roles_locked(void)
{
	int64_t now = os_monotonic_get_ns();

	if (g_arb.have_verdict && (now - g_arb.verdict_at_ns) < ARBITER_PRESENCE_POLL_NS) {
		return g_arb.provider_holds;
	}

	bool holds = query_provider_presence();
	if (g_arb.have_verdict && holds != g_arb.provider_holds) {
		U_LOG_W("input arbiter: provider hardware %s — hand roles move to %s.",
		        holds ? "present again" : "gone", holds ? "the provider" : "qwerty");
	}
	g_arb.provider_holds = holds;
	g_arb.have_verdict = true;
	g_arb.verdict_at_ns = now;
	return holds;
}

/*!
 * Recompute @ref t_input_arbiter::roles for the current verdict, bumping
 * `generation_id` only when the assignment actually changes. Caller holds
 * the mutex.
 */
static void
refresh_roles_locked(void)
{
	bool provider = provider_holds_roles_locked();

	int32_t left = provider ? g_arb.provider_left_index : g_arb.qwerty_left_index;
	int32_t right = provider ? g_arb.provider_right_index : g_arb.qwerty_right_index;

	// A pair that is only half-populated falls back per hand, so an
	// exotic provider that supplies one controller still leaves the
	// other hand usable from the keyboard.
	if (left < 0) {
		left = provider ? g_arb.qwerty_left_index : g_arb.provider_left_index;
	}
	if (right < 0) {
		right = provider ? g_arb.qwerty_right_index : g_arb.provider_right_index;
	}

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

	U_LOG_W("input arbiter: hand roles -> left='%s' right='%s' (%s, generation %u).",
	        left_xdev != NULL ? left_xdev->str : "<none>", right_xdev != NULL ? right_xdev->str : "<none>",
	        provider ? "input provider" : "qwerty fallback", (unsigned)generation);
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
	g_arb.provider_left_index = -1;
	g_arb.provider_right_index = -1;
	g_arb.qwerty_left_index = -1;
	g_arb.qwerty_right_index = -1;
	g_arb.roles = (struct xrt_system_roles)XRT_SYSTEM_ROLES_INIT;
	os_mutex_unlock(&g_arb_mutex);
}

void
t_input_arbiter_note_provider_pair(struct xrt_device *left, struct xrt_device *right)
{
	g_arb.provider_left = left;
	g_arb.provider_right = right;
}

void
t_input_arbiter_note_qwerty_pair(struct xrt_device *left, struct xrt_device *right)
{
	g_arb.qwerty_left = left;
	g_arb.qwerty_right = right;
}

bool
t_input_arbiter_provider_holds_roles(void)
{
	if (!g_arb_mutex_ready) {
		return query_provider_presence();
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

	bool have_provider = g_arb.provider_left != NULL || g_arb.provider_right != NULL;
	bool have_qwerty = g_arb.qwerty_left != NULL || g_arb.qwerty_right != NULL;
	if (!have_provider || !have_qwerty) {
		// Only one candidate exists, so there is nothing to arbitrate
		// between; the static roles the builder already assigned are
		// the right (and only) answer.
		U_LOG_I("input arbiter: not installed — %s.",
		        !have_provider ? "no input-provider devices (qwerty owns the hand roles)"
		                       : "no qwerty devices to fall back to");
		return;
	}

	os_mutex_lock(&g_arb_mutex);

	g_arb.xsysd = xsysd;
	g_arb.fallback_get_roles = xsysd->get_roles;
	g_arb.provider_left_index = index_of(xsysd, g_arb.provider_left);
	g_arb.provider_right_index = index_of(xsysd, g_arb.provider_right);
	g_arb.qwerty_left_index = index_of(xsysd, g_arb.qwerty_left);
	g_arb.qwerty_right_index = index_of(xsysd, g_arb.qwerty_right);

	// Force a fresh verdict: the builder's own query may be older than
	// the provider's startup grace.
	g_arb.have_verdict = false;
	refresh_roles_locked();

	xsysd->get_roles = arbiter_get_roles;

	os_mutex_unlock(&g_arb_mutex);

	U_LOG_W(
	    "input arbiter: installed — provider pair (%d,%d), qwerty pair (%d,%d); "
	    "roles re-resolve on every xrSyncActions.",
	    g_arb.provider_left_index, g_arb.provider_right_index, g_arb.qwerty_left_index, g_arb.qwerty_right_index);
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
