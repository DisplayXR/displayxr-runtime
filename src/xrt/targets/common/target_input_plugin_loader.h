// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime-side loader for input-provider plug-in DLLs.
 *
 * Consumer side of the ABI defined in `xrt/xrt_input_plugin.h` — the
 * second plug-in type (ADR-034), mirroring the display-processor loader
 * (`target_plugin_loader.h`). On first call, enumerates the platform's
 * input-provider discovery root — `HKLM\Software\DisplayXR\InputProviders\*`
 * on Windows, `NNN-<name>-input-provider.json` manifests in the SAME
 * directories the DP loader scans on POSIX — sorts by `ProbeOrder`
 * (ascending; missing = 100), and tries each in turn: load →
 * `xrtInputPluginNegotiate` → ABI-major gate (ADR-020 rule 3) →
 * `iface->probe()`. First successful probe wins and is cached for the
 * process lifetime (v1 = single active provider; composition deferred).
 *
 * No registered provider is NOT an error — the system builder falls back
 * to the qwerty-emulated motion controllers. Contract:
 * `docs/specs/runtime/input-provider-discovery.md`.
 *
 * The provider DLL handle is intentionally leaked for the process
 * lifetime, same rationale as the DP loader: the devices it created stay
 * callable until system teardown.
 *
 * Issue #823.
 *
 * @ingroup target_common
 */

#pragma once

#include "xrt/xrt_input_plugin.h"

#include <stdbool.h>
#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Descriptor for one registered input provider, as discovered from the
 * platform's discovery root WITHOUT loading the DLL/dylib/`.so`. Used by
 * `displayxr-cli input list`. All fields UTF-8.
 */
struct target_input_plugin_desc
{
	char id[64]; /* discovery id (Windows subkey / manifest <id>) */
	char display_name[128];
	char vendor[64];
	char version[64];
	char binary_path[512];
	uint32_t probe_order; /* lower wins; missing defaults to 100 */
	bool enabled;         /* Windows `Enabled` DWORD; true when absent */
};

/*!
 * Enumerate registered input providers WITHOUT loading them. Writes up to
 * @p max descriptors and returns the count (0 if the discovery root is
 * absent / empty). Re-reads the discovery root every call.
 */
int
target_input_plugin_enumerate(struct target_input_plugin_desc *out, int max);

/*!
 * How many providers loaded, ABI-passed and probed successfully.
 *
 * ADR-034 Amendment 3: the scan keeps EVERY claiming provider resident,
 * ordered by ProbeOrder ascending, rather than stopping at the first.
 * Which one drives the hands is a presence question the arbiter answers
 * continuously (`target_input_arbiter.h`) — a modality going away hands
 * the roles to the next-ranked one that is present, and only falls to
 * qwerty when none is. Runs discovery on first call.
 */
int
target_input_plugin_get_count(void);

/*!
 * The @p index'th provider's iface in priority order (0 = highest
 * priority), or NULL when @p index is out of range. Runs discovery on
 * first call; thread-safety mirrors `target_plugin_get_active` (first
 * call happens on the single-threaded `xrCreateInstance` path).
 */
const struct xrt_input_plugin_iface *
target_input_plugin_get_iface(int index);

/*!
 * The instance handle produced by the @p index'th provider's `probe()`,
 * or NULL. Pair with @ref target_input_plugin_get_iface.
 */
struct xrt_input_plugin_instance *
target_input_plugin_get_instance(int index);

/*!
 * The @p index'th provider's ProbeOrder — its arbitration priority,
 * ascending (lower wins), exactly the DP loader's convention. Returns
 * `UINT32_MAX` when @p index is out of range.
 */
uint32_t
target_input_plugin_get_priority(int index);

/*!
 * The highest-priority claiming provider, or NULL if none is registered
 * / claimed the system. Convenience alias for
 * `target_input_plugin_get_iface(0)`, kept for diagnostics
 * (`displayxr-cli`) and single-provider call sites. NOTE: this is the
 * highest-*ranked* provider, not necessarily the one currently holding
 * the hand roles — ask the arbiter for that.
 */
const struct xrt_input_plugin_iface *
target_input_plugin_get_active(void);

/*!
 * Returns the instance handle produced by the active provider's
 * `probe()`, or NULL. Pair with @ref target_input_plugin_get_active.
 */
struct xrt_input_plugin_instance *
target_input_plugin_get_active_instance(void);

/*!
 * Outcome of the (single, cached) discovery scan: how many registered
 * providers DECLINED cleanly, and how many could not be dispatched at all.
 *
 * The distinction matters to diagnostics. "Nobody claimed the system" is
 * a perfectly normal state — a provider declines when its hardware type
 * isn't on the box, and sim-input declines unless `DXR_SIM_INPUT` is set
 * — and then qwerty rightly keeps the hand roles. A provider that failed
 * to LOAD (missing entry point, ABI-major mismatch) is a real fault worth
 * failing a self-test over. Without this split the two are
 * indistinguishable from outside the loader.
 *
 * Runs discovery on first call, like @ref target_input_plugin_get_active.
 * Either out-param may be NULL.
 */
void
target_input_plugin_get_scan_result(int *out_declined, int *out_failed);

/*!
 * Read the ForceQwerty debug override — `HKLM\Software\DisplayXR\Input\
 * ForceQwerty` (DWORD != 0) on Windows, a `force_qwerty` file in the
 * per-user manifest dir on POSIX (first byte '0' = off, anything else =
 * on). When true, the builder skips input providers entirely and qwerty
 * claims the hand roles exactly as before ADR-034. Registry/config gate
 * by project convention — not an env var.
 */
bool
target_input_plugin_get_force_qwerty(void);

#ifdef __cplusplus
}
#endif
