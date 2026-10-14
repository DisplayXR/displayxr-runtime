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

#include "xrt/xrt_config_os.h"
#include "xrt/xrt_results.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if !defined(_WIN32) && !defined(__ANDROID__)
#include <limits.h>
#endif


#ifdef __cplusplus
extern "C" {
#endif


/*
 *
 * Routing + platform plumbing shared by every class (moved here from the
 * display-processor loader with ADR-054 N2; the `target_plugin_` names stay,
 * they are what every caller and every doc says).
 *
 */

/*!
 * Which class a POSIX discovery-root file belongs to (ADR-053: "the suffix is
 * the router"). All classes share one directory, so every loader classifies
 * with this one function: `NNN-<id>-input-provider.json` is INPUT,
 * `NNN-<id>-conversion-provider.json` is CONVERSION, any other `*.json` is
 * DISPLAY, anything else is NONE. Pure, compiled on every platform so it is
 * unit-testable everywhere.
 */
enum target_plugin_manifest_kind
{
	TARGET_PLUGIN_MANIFEST_NONE = 0,
	TARGET_PLUGIN_MANIFEST_DISPLAY,
	TARGET_PLUGIN_MANIFEST_INPUT,
	TARGET_PLUGIN_MANIFEST_CONVERSION,
};

enum target_plugin_manifest_kind
target_plugin_classify_manifest(const char *filename);

//! Android library prefix of a display-processor plug-in (`libdxrp<NNN>_<id>.so`).
#define TARGET_PLUGIN_ANDROID_PREFIX_DISPLAY "libdxrp"
//! Android library prefix of a conversion provider (`libdxrc<NNN>_<id>.so`, ADR-053).
#define TARGET_PLUGIN_ANDROID_PREFIX_CONVERSION "libdxrc"

/*!
 * Parse an Android component library name `<prefix><NNN>_<id>.so` into its
 * three-digit ProbeOrder and `<id>`. Returns false (outputs untouched except
 * possibly @p out_id on success only) on any malformed name, including a
 * different prefix. Pure; compiled on every platform for unit tests.
 */
bool
target_plugin_parse_android_filename(
    const char *name, const char *prefix, uint32_t *out_order, char *out_id, size_t id_size);

#if defined(_WIN32)
/*!
 * Pin the runtime core DLL (DisplayXRClient.dll) into the process by
 * absolute path so component DLL imports of it resolve independent of
 * cwd/PATH (issue #328). Idempotent, one-shot. Every class's binaries carry
 * that import; @ref target_component_load_binary calls this before each load.
 */
void
target_plugin_preload_runtime_core_dll(void);
#endif

#if !defined(_WIN32) && !defined(__ANDROID__)
/*!
 * Assemble the POSIX discovery roots (env override + per-user + system dirs)
 * in priority order; returns the count. Each root is a PATH_MAX-sized buffer.
 * Every class scans the SAME roots; the manifest suffix routes
 * (`docs/specs/runtime/plugin-discovery.md` §3).
 */
int
target_plugin_build_discovery_roots(char roots[][PATH_MAX], int max_roots);
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
#define TARGET_COMPONENT_FLAG_USER_OVERLAY (1u << 3)    //!< `enabled` came from the per-user (HKCU) overlay (N3a)

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

	/* Provenance (ADR-054 N4b). UNKNOWN until @ref target_component_check_provenance
	 * ran — the load path runs it; a lister calls it explicitly. */
	uint32_t provenance;         //!< enum target_component_provenance
	char provenance_reason[160]; //!< one line: why this verdict (allowlist row to write, signer, ...)
	char signer[128];            //!< leaf certificate CN when SIGNED; "" otherwise
	char sha256[65];             //!< hex digest of the binary's bytes when it could be read; "" otherwise

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

/*!
 * N3a (ADR-054 verb `enable|disable`): overlay the per-user `Enabled` onto
 * rows @ref target_component_enumerate returned. For every usable row, reads
 * `HKCU\<registry_root>\<id>\Enabled` and, where present, replaces the HKLM
 * value — HKCU wins — unless HKLM `<registry_root>\Policy\Lock` = 1, which
 * makes the machine value win for every row (the conversion class's rule,
 * conversion-provider-discovery.md §3.1). Only `Enabled` is ever read from
 * HKCU: a per-user key can never inject a binary. Keeps `flags` / `reason`
 * consistent (DISABLED set with "disabled for this user (HKCU Enabled = 0)",
 * or cleared) and marks each row it touched TARGET_COMPONENT_FLAG_USER_OVERLAY
 * so a class can say where `enabled` came from. Windows only; a no-op on
 * every other platform (there is no per-user `Enabled` store there — the
 * manifest's `enabled` is the switch). Returns the number of rows overlaid.
 */
int
target_component_apply_user_overlay(const struct target_component_class *cls,
                                    struct target_component_entry *entries,
                                    size_t n);

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

/*
 *
 * Provenance (ADR-054 N4b): is this binary one the machine's administrator
 * meant to run? A registration row needs admin once; the file it points at
 * does not, and nothing used to look at the file. Every class gets this check
 * in the shared load path. The gate is AUDIT-ONLY until `Trust\Enforce` is
 * set (PR A); PR B flips the default.
 *
 */

//! Why the trunk trusts (or does not trust) a component binary. Stable names: @ref target_component_provenance_str.
enum target_component_provenance
{
	TARGET_COMPONENT_PROVENANCE_UNKNOWN = 0,       //!< not checked (yet)
	TARGET_COMPONENT_PROVENANCE_SIGNED,            //!< valid Authenticode chain to a trusted root (Windows)
	TARGET_COMPONENT_PROVENANCE_ALLOWLISTED,       //!< its SHA-256 is in the admin-only allowlist
	TARGET_COMPONENT_PROVENANCE_PLATFORM_TRUSTED,  //!< inside the runtime APK / a root-owned system path (.deb)
	TARGET_COMPONENT_PROVENANCE_DEV_OVERRIDE,      //!< unsigned + unlisted, admitted by DXR_ALLOW_DEV_PLUGIN_PATHS on a non-elevated host
	TARGET_COMPONENT_PROVENANCE_UNVERIFIED,        //!< none of the above — what the gate refuses once enforced
};

//! "unknown" / "signed" / "allowlisted" / "platform-trusted" / "dev-override" / "unverified".
const char *
target_component_provenance_str(uint32_t p);

/*!
 * The verdict rule, pure, so it can be pinned by tests: first true wins —
 * SIGNED, ALLOWLISTED, PLATFORM_TRUSTED, then DEV_OVERRIDE only when
 * @p dev_override is set AND the process is not elevated (an elevated host
 * gets no override: ADR-054 N4b), else UNVERIFIED. @p reason (may be NULL)
 * receives the one-line why; @p sha256_hex (may be NULL) is quoted in the
 * UNVERIFIED reason as the allowlist row to write.
 */
uint32_t
target_component_provenance_classify(bool signed_ok,
                                     bool allowlisted,
                                     bool platform_trusted,
                                     bool dev_override,
                                     bool elevated,
                                     const char *sha256_hex,
                                     char *reason,
                                     size_t reason_cap);

/*!
 * Fill @p e's provenance fields for its `binary_path`: hash the file
 * (`sha256`), look the digest up in the admin-only allowlist
 * (`HKLM\Software\DisplayXR\Trust\Allowlist` value names on Windows,
 * `/etc/displayxr/trust/allowlist` lines on POSIX), check the Authenticode
 * chain (Windows), the platform rule (Android: always; .deb: root-owned,
 * not group/world-writable, under /usr or /opt), the dev override and the
 * host's elevation, then @ref target_component_provenance_classify. Results
 * are cached per path + size + mtime for the process, so a class that
 * re-enumerates on every world event does not re-hash. Returns true when the
 * verdict is anything but UNVERIFIED. Never loads the binary.
 */
bool
target_component_check_provenance(const struct target_component_class *cls, struct target_component_entry *e);

//! Hex SHA-256 of a file's bytes into @p out_hex (65 chars). False when unreadable.
bool
target_component_sha256_file(const char *path, char *out_hex, size_t cap);

//! Is this process elevated (Windows: token elevation; POSIX: euid 0)?
bool
target_component_process_elevated(void);

//! `DXR_ALLOW_DEV_PLUGIN_PATHS` is set (non-empty, not "0") — the same reading the #952 path guard uses.
bool
target_component_dev_override_set(void);

/*!
 * Is the provenance gate ENFORCING? Windows: `HKLM\Software\DisplayXR\Trust`
 * value `Enforce` (DWORD) != 0; POSIX: `/etc/displayxr/trust/enforce` exists.
 * Absent = audit mode (log, load anyway). PR B flips this default.
 */
bool
target_component_trust_enforced(void);

//! Outcome of @ref target_component_load_binary.
enum target_component_load_result
{
	TARGET_COMPONENT_LOAD_OK = 0,
	TARGET_COMPONENT_LOAD_BINARY_MISSING,     //!< the registered binary does not exist (orphan registration)
	TARGET_COMPONENT_LOAD_DEPENDENCY_MISSING, //!< binary exists, a library it imports does not (Windows)
	TARGET_COMPONENT_LOAD_FAILED,             //!< any other LoadLibrary / dlopen failure
	TARGET_COMPONENT_LOAD_PATH_REFUSED,       //!< #952 dev/build-tree path refused
	TARGET_COMPONENT_LOAD_NO_ENTRY_POINT,     //!< loaded, but no class entry point (another class's binary)
	TARGET_COMPONENT_LOAD_UNTRUSTED,          //!< provenance UNVERIFIED and the gate is enforcing (ADR-054 N4b)
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
                             struct target_component_entry *e,
                             void **out_module,
                             void **out_entrypoint,
                             uint32_t *out_os_error,
                             char *reason,
                             size_t reason_cap);

/*!
 * Log the breadcrumb and any failure at INFO instead of WARN. For a class
 * that re-probes the same binary on world events (the display-processor
 * loader re-attempts a declined vendor plug-in on every refresh, ADR-045):
 * one WARN per process for a binary that keeps failing the same way, not one
 * per retry. The ABI gate's lines are unaffected (they are a verdict, not a
 * retry).
 */
#define TARGET_COMPONENT_LOAD_FLAG_QUIET (1u << 0)

/*!
 * @ref target_component_load_binary with TARGET_COMPONENT_LOAD_FLAG_* flags.
 * Also runs @ref target_component_check_provenance on @p e (hence non-const)
 * between the #952 path guard and the load: in audit mode an UNVERIFIED
 * binary is logged (WARN, or INFO under QUIET) and loaded anyway; with the
 * gate enforcing it is refused with TARGET_COMPONENT_LOAD_UNTRUSTED.
 */
enum target_component_load_result
target_component_load_binary_ex(const struct target_component_class *cls,
                                struct target_component_entry *e,
                                uint32_t flags,
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
