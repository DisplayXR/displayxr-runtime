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
	bool enabled;         /* Windows `Enabled` DWORD (HKCU over HKLM) / manifest `enabled`; true when absent */
	/*!
	 * Where @ref enabled came from (N3a, ADR-054 verb `enable|disable`):
	 *  - "user": a per-user HKCU `Enabled` is in force, whatever its value
	 *    (`TARGET_COMPONENT_FLAG_USER_OVERLAY`) — so an explicit HKCU
	 *    `Enabled` = 1 reads "user";
	 *  - "machine": disabled by HKLM `Enabled` = 0 (Windows);
	 *  - "manifest": disabled by the POSIX manifest's `"enabled": false`;
	 *  - "default": enabled with no per-user value — which includes an
	 *    explicit HKLM `Enabled` = 1 / `"enabled": true`, because the shared
	 *    row reader records the value, not its presence.
	 */
	char enabled_source[12];
	/*! ADR-054 N4b: `enum target_component_provenance` of the binary, as
	 * @ref target_input_plugin_enumerate classified it (a lister checks
	 * explicitly; the verdict is cached per file for the process). */
	uint32_t provenance;
	char provenance_reason[160]; //!< one line: why that verdict
	char signer[128];            //!< leaf certificate CN when SIGNED; "" otherwise
};

/*!
 * Enumerate registered input providers WITHOUT loading them. Writes up to
 * @p max descriptors and returns the count (0 if the discovery root is
 * absent / empty). Re-reads the discovery root every call, and runs the N4b
 * provenance check on every listed binary (cached per file).
 */
int
target_input_plugin_enumerate(struct target_input_plugin_desc *out, int max);

/*!
 * The REGISTRATION id (registry subkey / manifest `id`) of the @p index'th
 * resident provider in priority order, or "" when out of range. Distinct
 * from `iface->id`, which is the binary's own: one DLL registered under two
 * ids (CI's `sim-input` + `sim-input-alt`) is two rows with one `iface->id`,
 * and `PreferredPlugin` names a ROW — so this is what says which row won
 * (`selftest --json` `input_providers.ranked`, N3b).
 */
const char *
target_input_plugin_get_row_id(int index);

struct target_component_class;

/*!
 * The input class's row in the shared component loader (ADR-054 N2): what
 * the one options contract keys on (`target_component_get_setting`,
 * `displayxr-cli input options|set|unset`, N3c). Static, never NULL.
 */
const struct target_component_class *
target_input_plugin_component_class(void);

struct target_component_setting_desc;

/*!
 * The settings provider @p id declares (N3c): its manifest `settings_schema`
 * (POSIX) merged with the binary's `describe_settings` slot, which means
 * loading + negotiating the registered binary (no probe) and unloading it
 * again. Returns the count written to @p out (up to @p max); 0 for an
 * unregistered id or a binary that declares nothing.
 */
int
target_input_plugin_describe_settings(const char *id, struct target_component_setting_desc *out, int max);

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
 * Providers the ADR-054 N4b provenance gate REFUSED at the last scan (unsigned,
 * not allowlisted, no admissible override) — not part of @p out_failed above,
 * so a verdict can name the gate instead of "failed to load".
 */
int
target_input_plugin_get_scan_untrusted(void);

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

/*!
 * ADR-054 N7 (#2143): the `EmulatedControllers` gate — may the qwerty
 * emulated controller PAIR exist at all? True (the default, absent = 1)
 * keeps today's floor: the pair is created, sits in the device list and
 * takes any hand role no provider holds. False: the pair is not published —
 * no qwerty left/right device, and the arbiter's floor becomes "no hand
 * roles" (both -1, `xrGetCurrentInteractionProfile` = XR_NULL_PATH) whenever
 * no provider is present. The qwerty head is NOT gated: it is the
 * look-around fallback, not a controller.
 *
 * Windows: `InputProviders\Policy\EmulatedControllers` (DWORD), HKCU first
 * (`displayxr-cli input emulated on|off`), then the HKLM 64-bit view
 * (`--machine`), HKCU ignored under `InputProviders\Policy\Lock` = 1 — the
 * shared loader's root-policy read, so the service's policy watch and the
 * N3 overlay rules cover it unchanged. POSIX: an `emulated_controllers` file
 * in the per-user manifest dir next to `force_qwerty` (first byte '0' = off,
 * anything else = on); no machine tier. Android: always on.
 *
 * @p out_source (may be NULL) receives a static string: "user" / "machine"
 * (the registry tier), "file" (the POSIX per-user file), or "default"
 * (nothing set it). Never the environment: this is class policy, not a
 * component setting, and it is read here — never via `getenv`.
 *
 * Orthogonal to ForceQwerty: ForceQwerty = 1 skips the providers,
 * EmulatedControllers = 0 withholds the pair; both together is a box with no
 * hand devices at all (the builder logs one WARN naming both).
 */
bool
target_input_plugin_get_emulated_controllers(const char **out_source);

/*!
 * Read the `PreferredPlugin` override (N3a, ADR-054 verb `prefer`; the
 * input twin of DP spec §2.1/§3.3): the id of the provider ranked FIRST,
 * ahead of ProbeOrder, for the hand/rig arbitration. Windows: HKCU
 * `Software\DisplayXR\InputProviders\PreferredPlugin`, then HKLM (what
 * `displayxr-cli input use <id> [--machine]` writes). POSIX:
 * `XRT_PREFERRED_INPUT_PLUGIN_ID` in the environment, then the
 * `input-preferred` file in the per-user manifest dir
 * (`target_component_user_dir`). A preferred provider that is
 * disabled, unregistered or declines falls through to normal ProbeOrder.
 * Writes "" and returns false when nothing is set.
 */
bool
target_input_plugin_get_preferred(char *out, size_t cap);

/*!
 * ADR-054 N3d (#2051): re-read the class's policy — `PreferredPlugin` (HKCU
 * then HKLM / the `input-preferred` file) and every registered row's
 * `Enabled` (HKLM with the HKCU overlay / the manifest) — and re-rank the
 * resident providers with it (@ref t_input_arbiter_rerank): the preferred
 * one to priority 0, the rest to their ProbeOrder, a disabled one withdrawn.
 * The loader's own ranked table follows, so `get_priority(i)` and `info`
 * agree with the arbiter. What it cannot do: load a provider that was
 * disabled (or unregistered) when this process scanned — the provider set is
 * fixed per process, so enabling one of those still takes a restart, which
 * the log says. Returns the number of candidates whose standing changed.
 * Runs discovery first if nothing did yet. Any thread.
 *
 * N7 (#2143): the `EmulatedControllers` gate is re-read too and applied the
 * same way — as a WITHDRAWAL of the qwerty floor candidate
 * (@ref t_input_arbiter_set_floor_withdrawn), never as a device add/remove:
 * a pair created at system build stays resident and is withdrawn (roles go
 * to -1 when no provider is present) or re-admitted at once; a pair NOT
 * created at build (the process started with the gate off) cannot appear
 * until the next start, which the log says. With no provider resident the
 * arbiter is not installed and the gate, like `Enabled`, applies at the next
 * start — logged once per process.
 */
int
target_input_plugin_reread_policy(void);

/*!
 * Publish the active display processor's nominal panel + viewer geometry to
 * input providers (#1380).
 *
 * The system builder calls this ONCE, right after the display plug-in has
 * answered `get_display_info` and BEFORE any provider's `create_devices`
 * runs — which is exactly the readiness the host-iface contract promises:
 * `get_display_geometry` returns
 * @ref XRT_ERROR_INPUT_HOST_GEOMETRY_NOT_READY before this call and the
 * cached record after it. @p geometry is copied; the caller keeps ownership.
 * NULL is ignored. The cache's own `struct_size` is always set to the
 * runtime's `sizeof`, whatever the caller left in it.
 *
 * Thread-safe: the publish is mutex-guarded against provider-thread reads,
 * even though in practice it is a one-shot write on the single-threaded
 * system-create path.
 */
void
target_input_plugin_set_display_geometry(const struct xrt_input_host_display_geometry *geometry);

/*!
 * THE process-lifetime host iface handed to every provider at
 * `xrtInputPluginNegotiate` (#1380).
 *
 * Providers are allowed to retain the pointer for the life of the process,
 * so this storage is static and never a stack local. Exposed for
 * diagnostics and tests, which drive `get_display_geometry` through the
 * very same function pointer a provider gets. Never NULL.
 */
const struct xrt_input_plugin_host_iface *
target_input_plugin_get_host_iface(void);

#ifdef __cplusplus
}
#endif
