// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The class-agnostic half of every component loader (ADR-054 N2).
 *
 * DisplayXR is a trunk with peer component classes plugged in over versioned
 * C ABIs (ADR-054). Each class — display processors (ADR-019/020), input
 * providers (ADR-034), conversion providers (ADR-053), the next one — owns
 * its negotiate symbol, its ABI major, its discovery root and its vtable.
 * What it does NOT own, and what used to be copied once per class, is the
 * flow that gets from "registered somewhere" to "a negotiate function I may
 * call": enumerate the discovery root, read the common row fields, sort by
 * ProbeOrder with a deterministic tie-break, shadow duplicate ids across
 * roots, refuse build-tree paths (#952), pin the runtime core DLL (#328),
 * sanitize-preload fragile dependencies (#434), `LoadLibrary` / `dlopen`,
 * resolve the entry point, and classify the negotiate outcome under the
 * ADR-020 rule-3 ABI gate.
 *
 * This file holds exactly that flow, driven by a per-class table row
 * (@ref target_component_class). A class loader is then a thin wrapper:
 * build its row, call @ref target_component_enumerate, and for each entry
 * call @ref target_component_load_binary, its own typed negotiate, and
 * @ref target_component_abi_gate. The class keeps whatever is class-specific
 * — the host iface it hands to negotiate, what `probe()` means, its
 * selection policy, its public descriptor struct.
 *
 * Discovery contract per platform (shared by every class; the suffix /
 * prefix / registry root is the router, ADR-053):
 *  - Windows: `HKLM\<registry_root>\<id>` rows with `Binary` (required),
 *    `DisplayName`, `Vendor`, `Version`, `ProbeOrder`, `Enabled` and a
 *    `Settings` subkey (the universal options contract, ADR-054 §2.7).
 *  - macOS / desktop Linux: `NNN-<id><manifest suffix>.json` manifests in
 *    the roots `target_plugin_build_discovery_roots` assembles. Per-user
 *    manifests shadow system ones by id.
 *  - Android: `<android_prefix><NNN>_<id>.so` next to the runtime library
 *    and under `XRT_PLUGIN_SEARCH_PATH`.
 *
 * Order is deterministic everywhere: ProbeOrder ascending, then id, then
 * source (#1466). Malformed rows are RETURNED (flagged, with a reason), not
 * dropped, so `displayxr-cli` can show a broken registration instead of
 * silently losing it; each class decides whether to list or skip them.
 *
 * @ingroup target_common
 */

#pragma once

#include "target_plugin_loader.h"

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_results.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


#ifdef __cplusplus
extern "C" {
#endif

//! Upper bound on registered components one enumeration returns, per class.
#define TARGET_COMPONENT_MAX_ENTRIES 16
//! UTF-8 path buffer size for binaries and manifests.
#define TARGET_COMPONENT_PATH_MAX 512
//! Upper bound on `Settings\*` / "settings" members carried per row.
#define TARGET_COMPONENT_MAX_SETTINGS 16
//! Bytes of class-private row data (@ref target_component_entry::extra).
#define TARGET_COMPONENT_EXTRA_BYTES 4352
//! ProbeOrder when the registry value / manifest field / file-name prefix is absent.
#define TARGET_COMPONENT_DEFAULT_PROBE_ORDER 100u

/* Entry flags (bitmask). */
#define TARGET_COMPONENT_FLAG_DISABLED (1u << 0)        //!< `Enabled` = 0 / "enabled": false
#define TARGET_COMPONENT_FLAG_MALFORMED (1u << 1)       //!< row / manifest unusable; see `reason`
#define TARGET_COMPONENT_FLAG_PROBE_ORDER_TIE (1u << 2) //!< shares its ProbeOrder with a neighbour; id broke the tie

//! One `Settings\<KEY>` value (Windows) / "settings" member (POSIX), as a string.
struct target_component_setting
{
	char key[64];
	char value[256];
};

/*!
 * One registered component of some class, as discovered — never loaded.
 * Plain data, ~11 KB: heap- or static-allocate arrays of these, never on a
 * hot thread's stack. All strings UTF-8, NUL-terminated.
 */
struct target_component_entry
{
	char id[64];                                 //!< registry subkey / manifest plugin.id / Android file-name id
	char display_name[128];                      //!< defaults to id when absent
	char vendor[64];                             //!< informational
	char version[64];                            //!< informational
	char binary_path[TARGET_COMPONENT_PATH_MAX]; //!< `Binary` / plugin.binary_path / the .so path
	char source[TARGET_COMPONENT_PATH_MAX];      //!< where it came from: registry key path or manifest path
	uint32_t probe_order;                        //!< lower wins; default TARGET_COMPONENT_DEFAULT_PROBE_ORDER
	bool enabled;                                //!< `Enabled` (absent = true)
	uint32_t flags;                              //!< TARGET_COMPONENT_FLAG_*
	uint32_t setting_count;                      //!< valid entries in settings[]
	struct target_component_setting settings[TARGET_COMPONENT_MAX_SETTINGS];
	char reason[192]; //!< why MALFORMED / DISABLED; "" otherwise
	//! Class-private row data filled by @ref target_component_class::read_extra
	//! (e.g. a conversion provider's Hosting / Supersedes / Packs). Zeroed
	//! before the hook runs; opaque to this file.
	uint8_t extra[TARGET_COMPONENT_EXTRA_BYTES];
};

//! What @p raw points at in @ref target_component_class::read_extra.
enum target_component_raw_kind
{
	TARGET_COMPONENT_RAW_NONE = 0,        //!< nothing to read from (Android file-name discovery)
	TARGET_COMPONENT_RAW_REGISTRY_KEY,    //!< Windows: `HKEY` of the open row subkey
	TARGET_COMPONENT_RAW_MANIFEST_PLUGIN, //!< POSIX: `const cJSON *` of the manifest's "plugin" object
};

struct target_component_class;

/*!
 * Optional per-row hook: read the class's own fields from the raw source into
 * @p e (typically its `extra` bytes). Call @ref target_component_mark_malformed
 * to reject the row. The common fields are already filled when it runs.
 */
typedef void (*target_component_read_extra_fn)(const struct target_component_class *cls,
                                               const void *raw,
                                               enum target_component_raw_kind raw_kind,
                                               struct target_component_entry *e);

/*!
 * One component class, as the shared loader sees it — the ADR-054 "per-class
 * table row". A class loader builds one of these (statically) and passes it
 * to every call here. Nothing in it is a vtable; the class's own ABI stays in
 * its own header.
 */
struct target_component_class
{
	//! Human name for messages: "display processor", "input provider", ...
	const char *name;
	//! Exact log-line prefix, kept stable because logs are grepped:
	//! "plugin loader", "input plugin loader", "conversion plugin loader".
	const char *log_tag;
	//! Negotiate export name: XRT_*_PLUGIN_ENTRYPOINT_NAME.
	const char *entrypoint;
	//! The class ABI major this runtime speaks: XRT_*_PLUGIN_API_VERSION_CURRENT.
	uint32_t abi_current;
	//! Windows discovery root under HKLM, e.g. "Software\\DisplayXR\\InputProviders".
	const char *registry_root;
	//! Which POSIX manifest suffix routes to this class.
	enum target_plugin_manifest_kind manifest_kind;
	//! Android library prefix ("libdxrp", "libdxrc"); NULL = this class has no Android discovery.
	const char *android_prefix;
	//! Optional: subkeys of the registry root that hold policy, not rows
	//! (e.g. conversion's `Policy`, `PreferredProvider`, `Prewarm`). Case-insensitive names.
	bool (*is_reserved_subkey)(const char *name);
	//! Optional: class-private row fields.
	target_component_read_extra_fn read_extra;
};

/*!
 * Enumerate every registered component of @p cls WITHOUT loading any. Writes
 * up to @p max entries, sorted (ProbeOrder ascending, then id, then source;
 * tie flags set), and returns the count. Re-reads the discovery root every
 * call. Malformed and disabled rows are returned too, flagged, with a reason.
 * Logs at INFO only.
 */
int
target_component_enumerate(const struct target_component_class *cls, struct target_component_entry *out, int max);

/*!
 * Zero @p e and apply the defaults (ProbeOrder, enabled).
 */
void
target_component_entry_init(struct target_component_entry *e);

/*!
 * Flag @p e MALFORMED with @p reason (copied). Idempotent; the first reason wins.
 */
void
target_component_mark_malformed(struct target_component_entry *e, const char *reason);

/*!
 * Sort ProbeOrder ascending, ties by id then source; sets
 * TARGET_COMPONENT_FLAG_PROBE_ORDER_TIE on both members of each tie and logs
 * one INFO per tied pair (#1466). Callers that want the vendor to see the tie
 * WARN on the flag themselves.
 */
void
target_component_sort(const struct target_component_class *cls, struct target_component_entry *entries, int count);

/*!
 * Append @p src (one discovery root's rows, in that root's order) to @p dst,
 * skipping any usable row whose id is already present among @p dst's usable
 * rows: earlier roots shadow later ones; within a root the first file wins.
 * Malformed rows never shadow and are never shadowed. Returns the new count.
 */
int
target_component_merge(const struct target_component_class *cls,
                       struct target_component_entry *dst,
                       int dst_count,
                       int max,
                       const struct target_component_entry *src,
                       int src_count);

/*!
 * Parse one manifest's JSON text into @p out (initialised here). @p source is
 * the manifest path, recorded in out->source; its `NNN-` file-name prefix is
 * the ProbeOrder fallback when the manifest has no `probe_order`. Returns true
 * for a usable manifest; false with MALFORMED + reason otherwise. out->id is
 * the manifest's id when it had one, else the file name. The class hook runs
 * on the "plugin" object.
 *
 * Schema (`docs/specs/runtime/plugin-discovery.md` §3): `{"file_format_version":
 * "1.0","plugin":{"id","display_name","vendor","version","binary_path",
 * "probe_order","enabled","settings":{...}, <class fields>}}`. `id` and
 * `binary_path` are required; `probe_order` must be a non-negative integer and
 * `enabled` a boolean when present.
 */
bool
target_component_parse_manifest_json(const struct target_component_class *cls,
                                     const char *json_text,
                                     const char *source,
                                     struct target_component_entry *out);

#ifdef _WIN32
/*!
 * Enumerate an arbitrary registry key with the exact row reader — HKCU when
 * @p current_user, else HKLM — WITHOUT the sort. Exists so tests can drive the
 * registry path from a throwaway HKCU key (a dev box must never get HKLM test
 * rows). Production uses @ref target_component_enumerate.
 */
int
target_component_enumerate_registry(const struct target_component_class *cls,
                                    bool current_user,
                                    const char *subkey,
                                    struct target_component_entry *out,
                                    int max);
#endif

#if !defined(_WIN32) && !defined(__ANDROID__)
/*!
 * The per-user manifest dir — the writable root for per-user overrides
 * (`preferred`, `force_qwerty`, ...). False when HOME and XDG are unset.
 */
bool
target_component_user_dir(char *out, size_t cap);
#endif


/*
 *
 * Binary loading + the ABI gate.
 *
 */

//! Outcome of @ref target_component_load_binary.
enum target_component_load_result
{
	TARGET_COMPONENT_LOAD_OK = 0,
	TARGET_COMPONENT_LOAD_BINARY_MISSING,     //!< the registered binary does not exist (orphan registration)
	TARGET_COMPONENT_LOAD_DEPENDENCY_MISSING, //!< binary exists, a library it imports does not (Windows)
	TARGET_COMPONENT_LOAD_FAILED,             //!< any other LoadLibrary / dlopen failure
	TARGET_COMPONENT_LOAD_PATH_REFUSED,       //!< #952 dev/build-tree path refused
	TARGET_COMPONENT_LOAD_NO_ENTRY_POINT,     //!< loaded, but no class entry point (another class's binary)
};

/*!
 * Load @p e's binary and resolve the class entry point. Runs, in order: the
 * exists check, the #952 path guard, the #328 runtime-core pin, the #434
 * sanitized preload, `LoadLibraryExW(LOAD_WITH_ALTERED_SEARCH_PATH)` /
 * `dlopen(RTLD_NOW | RTLD_LOCAL)`, `GetProcAddress` / `dlsym`. On OK writes
 * the module handle and the entry point (cast it to the class's negotiate
 * typedef). On any failure the module is unloaded again, @p out_os_error
 * (may be NULL) holds the Win32 error / 0, and @p reason (may be NULL) a
 * one-line why. Logs the #434 "loading binary" breadcrumb and one WARN per
 * failure under the class log tag.
 */
enum target_component_load_result
target_component_load_binary(const struct target_component_class *cls,
                             const struct target_component_entry *e,
                             void **out_module,
                             void **out_entrypoint,
                             uint32_t *out_os_error,
                             char *reason,
                             size_t reason_cap);

//! FreeLibrary / dlclose a module from @ref target_component_load_binary. NULL is a no-op.
void
target_component_unload(void *module);

//! Short stable name of a @ref target_component_load_result.
const char *
target_component_load_result_str(enum target_component_load_result r);

//! Outcome of @ref target_component_abi_gate.
enum target_component_abi_verdict
{
	TARGET_COMPONENT_ABI_OK = 0,
	TARGET_COMPONENT_ABI_DECLINED,         //!< negotiate returned XRT_ERROR_PROBER_NOT_SUPPORTED (not my platform)
	TARGET_COMPONENT_ABI_MISMATCH,         //!< plugin_api != abi_current (ADR-020 rule 3) — never dispatch
	TARGET_COMPONENT_ABI_NEGOTIATE_FAILED, //!< any other error, or success with no iface
};

/*!
 * Classify what a class's negotiate call returned (ADR-020 rule 3), the same
 * way for every class. @p xret, @p iface and @p plugin_version are what
 * negotiate produced. The mismatch test runs BEFORE the error test when the
 * component wrote a version, because a component of another major may well
 * refuse our version with an error AND report its own; a success that wrote
 * no version is a mismatch too (the vtable is unknown). Logs one WARN (ERROR
 * for a mismatch) per call under the class log tag; @p reason (may be NULL)
 * receives the one-line why for diagnostics.
 */
enum target_component_abi_verdict
target_component_abi_gate(const struct target_component_class *cls,
                          const char *id,
                          xrt_result_t xret,
                          const void *iface,
                          uint32_t plugin_version,
                          char *reason,
                          size_t reason_cap);

#ifdef __cplusplus
}
#endif
