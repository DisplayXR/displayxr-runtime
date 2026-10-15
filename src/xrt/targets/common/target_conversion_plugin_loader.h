// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Runtime-side discovery + negotiation for conversion-provider plug-ins.
 *
 * Consumer side of the ABI in `xrt/xrt_conversion_plugin.h` -- the third
 * plug-in type (ADR-053), next to the display-processor loader
 * (`target_plugin_loader.h`) and the input-provider loader
 * (`target_input_plugin_loader.h`). This file only FINDS and NEGOTIATES
 * providers; it never probes, describes, warms or selects. That is the
 * conversion core's job (#2000), which consumes the entry array below.
 *
 * Discovery (docs/specs/runtime/conversion-provider-discovery.md §3):
 *  - Windows: `HKLM\Software\DisplayXR\ConversionProviders\<id>` with
 *    `Binary` (required), `DisplayName`, `Vendor`, `Version`, `ProbeOrder`,
 *    `Enabled`, `Hosting`, `Supersedes`, `Packs` (REG_MULTI_SZ) and a
 *    `Settings` subkey. Every value is read and stored; `Hosting`,
 *    `Supersedes`, `Packs` and `Settings` are carried for the core.
 *  - macOS / desktop Linux: `NNN-<id>-conversion-provider.json` manifests in
 *    the SAME roots the DP loader scans (`target_plugin_build_discovery_roots`:
 *    `XRT_PLUGIN_SEARCH_PATH`, then per-user, then the installed dirs).
 *    Per-user manifests shadow system ones by id.
 *  - Android: `libdxrc<NNN>_<id>.so` in `XRT_PLUGIN_SEARCH_PATH` and the
 *    runtime `.so`'s own lib dir (the runtime APK's `jniLibs/<abi>/`).
 *
 * The suffix / prefix / registry root is the router: the DP and input
 * loaders never see a conversion provider and this loader never sees a DP
 * or input plug-in (see @ref target_plugin_classify_manifest).
 *
 * Order is deterministic: ProbeOrder ascending, then id (strcmp), with one
 * WARN per tie (the #1466 rule).
 *
 * Kill switches (§3.2), read with CRT getenv in the calling process:
 * `DXR_CONV_PROVIDERS=0` suppresses every row; `DXR_CONV_EXCLUSIVE=<id>`
 * suppresses every row but `<id>`. Suppressed rows are still enumerated
 * (so diagnostics can list them) but @ref target_conversion_plugin_negotiate_one
 * refuses them.
 *
 * @ingroup target_common
 */

#pragma once

#include "xrt/xrt_conversion_plugin.h"
#include "xrt/xrt_results.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

//! Upper bound on registered providers one enumeration returns.
#define XRT_CONV_LOADER_MAX_ENTRIES 16
//! Upper bound on model-pack manifests carried per provider (`Packs`).
#define XRT_CONV_LOADER_MAX_PACKS 8
//! Upper bound on settings carried per provider (`Settings\*` / "settings").
#define XRT_CONV_LOADER_MAX_SETTINGS 16
//! Path buffer size (UTF-8) for binaries, manifests and packs.
#define XRT_CONV_LOADER_PATH_MAX 512
//! ProbeOrder when the registry value / manifest field is absent.
#define XRT_CONV_LOADER_DEFAULT_PROBE_ORDER 100u

/*!
 * `Hosting` (§3.1). Unknown strings map to AUTO and set
 * @ref XRT_CONV_ENTRY_FLAG_HOSTING_UNKNOWN; the raw string is kept too.
 */
#define XRT_CONV_HOSTING_AUTO 0u
#define XRT_CONV_HOSTING_IN_SERVICE 1u
#define XRT_CONV_HOSTING_SATELLITE 2u

/* Entry flags (bitmask). */
#define XRT_CONV_ENTRY_FLAG_DISABLED (1u << 0)        //!< `Enabled` = 0 / "enabled": false
#define XRT_CONV_ENTRY_FLAG_SUPPRESSED (1u << 1)      //!< DXR_CONV_PROVIDERS=0 or DXR_CONV_EXCLUSIVE names another id
#define XRT_CONV_ENTRY_FLAG_PROBE_ORDER_TIE (1u << 2) //!< shares its ProbeOrder with a neighbour; id broke the tie
#define XRT_CONV_ENTRY_FLAG_HOSTING_UNKNOWN (1u << 3) //!< `Hosting` held an unrecognised string; treated as auto
#define XRT_CONV_ENTRY_FLAG_MALFORMED (1u << 4)       //!< row / manifest unusable; see `reason`

/*!
 * Where an entry stands. Enumeration leaves every usable row at NOT_LOADED
 * (or MALFORMED / DISABLED / SUPPRESSED); negotiation moves it on.
 */
enum xrt_conversion_plugin_load_status
{
	XRT_CONV_LOAD_NOT_LOADED = 0,   //!< discovered; not negotiated (yet)
	XRT_CONV_LOAD_NEGOTIATED,       //!< loaded, entry point found, ABI major matched; iface valid
	XRT_CONV_LOAD_MALFORMED,        //!< registry row / manifest unusable (missing Binary, bad JSON, ...)
	XRT_CONV_LOAD_DISABLED,         //!< Enabled = 0
	XRT_CONV_LOAD_SUPPRESSED,       //!< kill switch (DXR_CONV_PROVIDERS / DXR_CONV_EXCLUSIVE)
	XRT_CONV_LOAD_PATH_REFUSED,     //!< #952 dev-path guard refused the binary
	XRT_CONV_LOAD_LOAD_FAILED,      //!< LoadLibrary / dlopen failed
	XRT_CONV_LOAD_NO_ENTRYPOINT,    //!< no xrtConversionPluginNegotiate (e.g. a DP or input DLL)
	XRT_CONV_LOAD_DECLINED,         //!< negotiate returned XRT_ERROR_PROBER_NOT_SUPPORTED
	XRT_CONV_LOAD_NEGOTIATE_FAILED, //!< negotiate failed, or returned no / a too-small iface
	XRT_CONV_LOAD_ABI_MISMATCH,     //!< plugin_api != XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT
	XRT_CONV_LOAD_UNTRUSTED,        //!< ADR-054 N4b: the provenance gate refused the binary (unsigned, not allowlisted)
};

//! One `Settings\<KEY>` value (Windows) / "settings" member (POSIX), as a string.
struct xrt_conversion_plugin_setting
{
	char key[64];
	char value[256];
};

/*!
 * One registered conversion provider, as discovered and (optionally)
 * negotiated. Plain data, roughly 11 KB: the core owns the array (heap or
 * static), never the stack of a hot thread. All strings are UTF-8 and
 * NUL-terminated.
 */
struct xrt_conversion_plugin_entry
{
	/* Discovery. */
	char id[64];                                //!< registry subkey / manifest plugin.id / Android filename id
	char display_name[128];                     //!< defaults to id when absent
	char vendor[64];                            //!< informational
	char version[64];                           //!< informational (installers write it LAST, §3.8)
	char binary_path[XRT_CONV_LOADER_PATH_MAX]; //!< `Binary` / plugin.binary_path / the .so path
	char source[XRT_CONV_LOADER_PATH_MAX];      //!< where it came from: registry key or manifest path
	uint32_t probe_order;                       //!< lower wins; default XRT_CONV_LOADER_DEFAULT_PROBE_ORDER
	bool enabled;                               //!< `Enabled` (absent = true)
	uint32_t flags;                             //!< XRT_CONV_ENTRY_FLAG_*

	uint32_t hosting;     //!< XRT_CONV_HOSTING_* (carried for the core; P1 honours in-service only)
	char hosting_raw[32]; //!< the string as registered ("" when absent)
	char supersedes[64];  //!< e.g. "dp-lift" (carried for the core, §4.5)

	uint32_t pack_count;                                             //!< valid entries in packs[]
	char packs[XRT_CONV_LOADER_MAX_PACKS][XRT_CONV_LOADER_PATH_MAX]; //!< model-pack manifest paths
	uint32_t setting_count;                                          //!< valid entries in settings[]
	struct xrt_conversion_plugin_setting settings[XRT_CONV_LOADER_MAX_SETTINGS];

	/* Negotiation (filled by target_conversion_plugin_negotiate_one). */
	uint32_t status;                                 //!< enum xrt_conversion_plugin_load_status
	xrt_result_t negotiate_result;                   //!< what negotiate returned (XRT_SUCCESS if never called)
	uint32_t plugin_api_version;                     //!< what the provider reported (0 if never called)
	const struct xrt_conversion_plugin_iface *iface; //!< non-NULL only when status == NEGOTIATED
	void *module;                                    //!< HMODULE / dlopen handle; opaque
	char reason[192];                                //!< human-readable why, for any non-OK status
};

/*!
 * Enumerate registered conversion providers WITHOUT loading them. Writes up
 * to @p max entries, sorted (ProbeOrder ascending, then id), and returns the
 * count. Re-reads the discovery root every call (the core's re-describe
 * triggers re-run it). Malformed rows are returned too, with
 * status = MALFORMED and a reason, so diagnostics can show them; disabled /
 * suppressed rows likewise. Logs at INFO only -- the core WARNs once per
 * change, never per poll.
 */
int
target_conversion_plugin_enumerate(struct xrt_conversion_plugin_entry *out, int max);

/*!
 * Load @p e's binary, resolve `xrtConversionPluginNegotiate`, negotiate and
 * apply the ABI-major gate (ADR-020 rule 3). Does NOT probe or describe.
 *
 * @p host is the process-lifetime host iface the provider may retain; NULL
 * selects @ref target_conversion_plugin_default_host. On success: status =
 * NEGOTIATED, iface + module set, returns true. On any failure the binary is
 * unloaded again and status / reason say why. Rows that are MALFORMED,
 * DISABLED or SUPPRESSED are refused without touching the binary.
 * One WARN (or ERROR for an ABI mismatch) per call: call it once per change.
 */
bool
target_conversion_plugin_negotiate_one(struct xrt_conversion_plugin_entry *e,
                                       const struct xrt_conversion_plugin_host_iface *host);

/*!
 * The negotiation gate on its own, given an already-resolved entry point:
 * calls @p negotiate, classifies the outcome (DECLINED / NEGOTIATE_FAILED /
 * ABI_MISMATCH / NEGOTIATED) into @p e. Never loads or unloads anything and
 * never touches e->module. Used by @ref target_conversion_plugin_negotiate_one
 * after the symbol lookup; exposed so the gate is unit-testable with a stub.
 */
bool
target_conversion_plugin_negotiate_with(struct xrt_conversion_plugin_entry *e,
                                        xrt_conversion_plugin_negotiate_fn_t negotiate,
                                        const struct xrt_conversion_plugin_host_iface *host);

/*!
 * Unload a negotiated entry's binary and reset it to NOT_LOADED. Only for a
 * provider with no live object left (never probed, or every instance
 * destroyed): code from the binary must not run after this. In-service
 * providers are normally leaked for the process lifetime (§4.1); this exists
 * for declines, `conv probe`, tests and the satellite host. Safe on an entry
 * that was never loaded.
 */
void
target_conversion_plugin_unload(struct xrt_conversion_plugin_entry *e);

/*!
 * A process-lifetime host iface whose callbacks all answer "nothing"
 * (get_* / model_* return false, notify_changed is a no-op). For headless
 * tooling and tests; the conversion core passes its own. Never NULL.
 */
const struct xrt_conversion_plugin_host_iface *
target_conversion_plugin_default_host(void);

struct target_component_class;

/*!
 * The conversion class's row in the shared component loader (ADR-054 N2):
 * what the one options contract keys on (`target_component_get_setting`,
 * `displayxr-cli conv options|set|unset`, N3c). Static, never NULL.
 */
const struct target_component_class *
target_conversion_plugin_component_class(void);

struct target_component_setting_desc;

/*!
 * The settings provider @p id declares (N3c): its manifest `settings_schema`
 * (POSIX) merged with the binary's `describe_settings` slot, which means
 * loading + negotiating the registered binary with the default host (no
 * probe, no describe_cap) and unloading it again. Returns the count written
 * to @p out (up to @p max); 0 for an unregistered id or a binary that
 * declares nothing.
 */
int
target_conversion_plugin_describe_settings(const char *id, struct target_component_setting_desc *out, int max);

/*
 *
 * Pure helpers -- compiled on every platform, used by the discovery passes and
 * pinned by tests/tests_conversion_loader.cpp.
 *
 */

/*!
 * Sort ProbeOrder ascending, ties by id (strcmp); sets
 * XRT_CONV_ENTRY_FLAG_PROBE_ORDER_TIE on both members of each tie and logs one
 * INFO per tied pair (the core WARNs once per change).
 */
void
target_conversion_plugin_sort(struct xrt_conversion_plugin_entry *entries, int count);

/*!
 * ADR-054 N4b, for a LISTER (`displayxr-cli conv list`): classify the
 * provenance of the provider binary at @p binary_path the way the shared
 * load path does (`target_component_check_provenance`, cached per file for
 * the process), without loading it. The status rows the CLI prints come over
 * DIAG IPC and carry no verdict, so the lister asks here. Writes the
 * `enum target_component_provenance` value, the one-line reason and the
 * signer (any out-param may be NULL); returns true when the verdict is
 * anything but UNVERIFIED. An empty path yields UNKNOWN / false.
 */
bool
target_conversion_plugin_check_provenance(const char *binary_path,
                                          uint32_t *out_provenance,
                                          char *out_reason,
                                          size_t reason_cap,
                                          char *out_signer,
                                          size_t signer_cap);

/*!
 * Parse one POSIX manifest's JSON text into @p out (zeroed first). @p source
 * is the manifest path (recorded in out->source; its `NNN-` prefix supplies
 * the ProbeOrder when the manifest has none). Returns true for a usable
 * manifest. On failure returns false with out->status = MALFORMED, the
 * MALFORMED flag and out->reason set; out->id is the manifest's id when it
 * had one, else the file name.
 *
 * Schema: `{"file_format_version":"1.0","plugin":{"id","display_name",
 * "vendor","version","binary_path","probe_order","enabled","hosting",
 * "supersedes","packs":[...],"settings":{...}}}` -- the DP manifest plus the
 * optional conversion fields (§3.5). `id` and `binary_path` are required.
 */
bool
target_conversion_plugin_parse_manifest_json(const char *json_text,
                                             const char *source,
                                             struct xrt_conversion_plugin_entry *out);

/*!
 * Append @p src (one discovery root's entries, in that root's order) to
 * @p dst, skipping any usable entry whose id is already present among
 * @p dst's usable entries: earlier roots shadow later ones, and within a root
 * the first file wins. Malformed entries never shadow and are never shadowed.
 * Returns the new count (at most @p max).
 */
int
target_conversion_plugin_merge(struct xrt_conversion_plugin_entry *dst,
                               int dst_count,
                               int max,
                               const struct xrt_conversion_plugin_entry *src,
                               int src_count);

/*!
 * Apply the §3.2 kill switches to @p entries: with @p providers_env == "0"
 * every row is suppressed; with a non-empty @p exclusive_id every row whose
 * id differs is. NULL means unset. @ref target_conversion_plugin_enumerate
 * calls this with getenv("DXR_CONV_PROVIDERS") / getenv("DXR_CONV_EXCLUSIVE").
 */
void
target_conversion_plugin_apply_kill_switches(struct xrt_conversion_plugin_entry *entries,
                                             int count,
                                             const char *providers_env,
                                             const char *exclusive_id);

/*!
 * `Hosting` string to XRT_CONV_HOSTING_* (case-insensitive: "auto",
 * "in-service", "satellite"; NULL / "" = auto). @p out_known, when non-NULL,
 * is false for an unrecognised string.
 */
uint32_t
target_conversion_plugin_parse_hosting(const char *s, bool *out_known);

/*!
 * Subkeys of the `ConversionProviders` root that hold runtime POLICY, not a
 * provider row (discovery spec §3.1, "Root values"): `Policy\Lock`,
 * `PreferredProvider\<task>`, `Prewarm\<task>`. The registry reader skips
 * them, case-insensitively (registry names are), so they are never listed as
 * malformed providers -- and a provider can never be registered under one of
 * these ids. One table, shared with the CLI's `conv` code. NULL-terminated.
 */
#define XRT_CONV_RESERVED_SUBKEYS_INIT {"Policy", "PreferredProvider", "Prewarm", NULL}

//! The reserved-subkey table (see XRT_CONV_RESERVED_SUBKEYS_INIT), NULL-terminated.
extern const char *const target_conversion_plugin_reserved_subkeys[];

//! True when @p name (UTF-8) is a reserved policy subkey, case-insensitively.
bool
target_conversion_plugin_is_reserved_subkey(const char *name);

//! Short name of an enum xrt_conversion_plugin_load_status value.
const char *
target_conversion_plugin_status_str(uint32_t status);

#ifdef _WIN32
/*!
 * Enumerate an arbitrary registry key with the exact Windows row reader --
 * HKCU when @p current_user, else HKLM -- WITHOUT the kill switches or the
 * sort. Exists so tests can drive the registry path from a throwaway HKCU key
 * (a dev box must never get HKLM test rows). Production uses
 * @ref target_conversion_plugin_enumerate.
 */
int
target_conversion_plugin_enumerate_registry_key(bool current_user,
                                                const char *subkey,
                                                struct xrt_conversion_plugin_entry *out,
                                                int max);
#endif

#ifdef __cplusplus
}
#endif
