// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Conversion-provider diagnostics without a service (ADR-053 §7):
 *         enumerate -> negotiate -> probe -> describe -> select, NO model load.
 *
 * The one headless pass behind `displayxr-cli selftest` (check
 * `conversion_providers`, exit 13), `displayxr-cli conv list|status` when no
 * service is reachable (`source: headless`) and the headless status snapshot's
 * `conv_providers[]` / `conv_tasks[]` rows. It calls only the provider's [C]
 * slots plus `probe` (spec §2.3: <= 100 ms, no model load, no network) and
 * `destroy`; never `warm`, never a stream or a job. "The CLI never loads a
 * second model instance" (spec §7.1).
 *
 * The rows it produces are the snapshot's own row types
 * (`struct xrt_status_conv_provider` / `struct xrt_status_conv_task`), so the
 * headless and the service paths print through the same serialisers.
 *
 * The legacy `dp-lift` adapter row (spec §4.5) is described STATICALLY here:
 * declared when the active display plug-in exposes the lift-only factory,
 * without calling it. The service's adapter (#2003) replaces this with its
 * live row; until then this is the only place a dp-lift row exists.
 *
 * @ingroup target_common
 */

#pragma once

#include "xrt/xrt_display_status.h"
#include "xrt/xrt_conversion_plugin.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct xrt_plugin_iface;
struct xrt_conversion_plugin_entry;

//! Task slots a policy carries (XRT_CONV_TASK_NONE .. RESERVED_FIRST - 1).
#define TARGET_CONV_POLICY_TASKS 10
//! Per-user `Enabled` overrides a policy carries.
#define TARGET_CONV_POLICY_MAX_OVERRIDES 16

//! Registry root (HKLM and HKCU) of everything conversion (spec §3.1).
#define TARGET_CONV_REG_ROOT "Software\\DisplayXR\\ConversionProviders"

/*!
 * The effective §3.1 policy, already resolved (HKCU over HKLM unless
 * `Policy\Lock`). Windows reads the registry; POSIX leaves it empty (the
 * manifests carry `enabled`; root policy on POSIX is a later phase).
 */
struct target_conv_policy
{
	bool policy_lock;         //!< HKLM `Policy\Lock` = 1
	bool allow_research_only; //!< resolved `AllowResearchOnly`
	//! Resolved `PreferredProvider\<task>` per XRT_CONV_TASK_* ("" = none).
	char preferred[TARGET_CONV_POLICY_TASKS][96];
	//! Per-user `ConversionProviders\<id>\Enabled` overrides (HKCU; ignored under Policy\Lock).
	uint32_t override_count;
	struct
	{
		char id[64];
		bool enabled;
	} overrides[TARGET_CONV_POLICY_MAX_OVERRIDES];
};

/*!
 * Result of one headless pass. Plain data; nothing in it points into a
 * provider binary (every binary is unloaded again before the pass returns).
 */
struct target_conv_status
{
	uint32_t provider_count;
	struct xrt_status_conv_provider providers[XRT_STATUS_MAX_CONV_PROVIDERS];
	uint32_t task_count;
	struct xrt_status_conv_task tasks[XRT_STATUS_MAX_CONV_TASKS];

	uint32_t registered;     //!< registry rows / manifests enumerated (incl. malformed / disabled)
	uint32_t negotiated;     //!< providers that negotiated at ABI 1
	uint32_t malformed_rows; //!< registry rows / manifests unusable (reported, never a selftest failure)

	//! Capabilities rejected by descriptor validation (spec §7.2), across
	//! providers, plus out-of-range get_state answers. Non-zero = selftest exit 13.
	uint32_t bad_descriptors;
	char bad_note[192]; //!< "<id>: <first error>" of the first offender ("" = none)

	//! The static dp-lift row was declared (active plug-in has the lift-only factory).
	bool dp_lift_declared;
	char dp_lift_note[160];

	//! #2029: the Â§3.2 kill switch that emptied this pass ("DXR_CONV" /
	//! "DXR_LIFT"; "" = none). Set only by target_conv_status_build_headless.
	char kill_switch[16];

	struct target_conv_policy policy; //!< what selection ran with
};

/*!
 * Discovery spec Â§3.2 (#2029): `DXR_CONV=0` -- no conversion service at all --
 * or its alias `DXR_LIFT=0`, in THIS process's environment (the service reads
 * its own; a variable set on a client never reaches it). Returns the name of
 * the variable that engaged it ("DXR_CONV" wins when both are set), or NULL.
 */
const char *
target_conv_kill_switch(void);

/*!
 * Read the effective policy (Windows registry; POSIX: zeroed). Never fails;
 * an unreadable value is "absent".
 */
void
target_conv_policy_read(struct target_conv_policy *out);

/*!
 * The per-user `Enabled` override @p policy carries for @p id: 1 (HKCU
 * Enabled = 1), 0 (HKCU Enabled = 0), or -1 (none, or `Policy\Lock` makes
 * the machine value win). What the headless pass and the service's live
 * rows (#2051) both apply over the loader's HKLM `enabled`.
 */
int
target_conv_policy_enabled_override(const struct target_conv_policy *policy, const char *id);

/*!
 * The headless pass over the registered providers
 * (@ref target_conversion_plugin_enumerate) plus the static dp-lift row of
 * @p active_dp (may be NULL: no dp-lift row). Reads the policy itself. With
 * the kill switch engaged (@ref target_conv_kill_switch) it enumerates and
 * loads nothing: zero rows, @ref target_conv_status::kill_switch set.
 * Every negotiated binary is probed, described, destroyed and unloaded again.
 */
void
target_conv_status_build_headless(const struct xrt_plugin_iface *active_dp, struct target_conv_status *out);

/*!
 * The same pass over caller-supplied entries (tests hand stub DLLs in here;
 * the registry is never touched). @p entries are negotiated in place and
 * unloaded again before return. @p policy may be NULL (empty policy).
 */
void
target_conv_status_build_from_entries(struct xrt_conversion_plugin_entry *entries,
                                      int count,
                                      const struct xrt_plugin_iface *active_dp,
                                      const struct target_conv_policy *policy,
                                      struct target_conv_status *out);

/*!
 * The host iface the headless pass negotiates providers with: the loader's
 * default host, plus a get_setting that answers `DXR_CONV_<ID>_<KEY>` from the
 * environment, then the provider's registry `Settings` / manifest "settings".
 * It answers only while @ref target_conv_status_build_from_entries is running
 * that provider (single-threaded; the callbacks carry no provider context).
 * Exposed so a caller can pre-negotiate an entry (status NEGOTIATED, iface set);
 * the pass then probes + describes it without loading anything.
 */
const struct xrt_conversion_plugin_host_iface *
target_conv_status_headless_host(void);

struct conv_registry;

/*!
 * ADR-053 (#2005): the service's LIVE rows. Same row builders as the headless
 * pass, over a copy of the running conversion core's registry
 * (`conv_core_copy_registry`): its readiness is the core's, not a fresh
 * get_state. Provider identity (display name, vendor, version, loader status,
 * ABI) comes from @p entries matched by id (may be NULL / 0; a provider with
 * no entry reads BUILT_IN). Never calls a provider.
 */
void
target_conv_status_from_registry(const struct conv_registry *reg,
                                 const struct xrt_conversion_plugin_entry *entries,
                                 int count,
                                 const struct target_conv_policy *policy,
                                 struct target_conv_status *out);

/*!
 * What is bound to one task row (the service: `conv_core_row_activity`).
 * @p provider_id is the row's provider; @p composed selects the composed row.
 */
typedef void (*target_conv_activity_fn)(
    void *ud, const char *provider_id, uint32_t cap_id, bool composed, uint32_t *out_streams, uint32_t *out_jobs);

/*!
 * #2029: fill every task row's `streams` / `jobs_queued` (service only; the
 * headless pass has neither and leaves them 0) by asking @p fn per row.
 */
void
target_conv_status_fill_activity(struct target_conv_status *st, target_conv_activity_fn fn, void *ud);

/*!
 * Copy the rows into @p snap (`conv_providers[]`, `conv_tasks[]`). Does not
 * derive warnings: call `u_status_warnings_derive` after.
 */
void
target_conv_status_to_snapshot(const struct target_conv_status *st, struct xrt_status_snapshot *snap);

/*!
 * A registry subkey name under the root that is policy, not a provider
 * (`Policy`, `PreferredProvider`, `Prewarm`). The loader does not know these
 * yet, so the headless pass drops such rows instead of listing them as
 * malformed providers.
 */
bool
target_conv_is_policy_subkey(const char *name);

#ifdef __cplusplus
}
#endif
