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
 * `NNN-<id>-conversion-provider.json` is CONVERSION, a `<pack>.models.json`
 * model pack (ADR-053 P4, shipped beside its provider) is NONE, any other
 * `*.json` is DISPLAY, anything else is NONE. Pure, compiled on every
 * platform so it is unit-testable everywhere.
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

/*!
 * The component classes the trunk knows (ADR-054 §2), as a stable id — what a
 * class-generic verb or wire call names when it says "this class": the
 * `system_request_component_reselect` DIAG call, the service's policy-store
 * watch (`service_component_watch.h`). Never renumbered: the value crosses
 * the IPC boundary. 0 is reserved for "no class".
 */
enum target_component_class_id
{
	TARGET_COMPONENT_CLASS_NONE = 0,
	TARGET_COMPONENT_CLASS_DISPLAY = 1,    //!< display processors (ADR-019/020)
	TARGET_COMPONENT_CLASS_INPUT = 2,      //!< input providers (ADR-034)
	TARGET_COMPONENT_CLASS_CONVERSION = 3, //!< conversion providers (ADR-053)
	TARGET_COMPONENT_CLASS_COUNT = 4,      //!< one past the last valid id
};

//! "display" / "input" / "conversion" / "none"; never NULL.
const char *
target_component_class_id_str(uint32_t class_id);

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
	char provenance_reason[224]; //!< one line: why this verdict — long enough for the elevated-host text + the 64-hex allowlist row
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
	//! The `<CLASS>` token of the class's environment override
	//! `DXR_<CLASS>_<ID>_<KEY>` (N3c): "DP" / "INPUT" / "CONV". NULL = the
	//! class has no environment tier (test rows).
	const char *env_class;
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
 * (`preferred`, `force_qwerty`, `emulated_controllers`, `<id>.settings.json`,
 * ...). False when HOME and XDG are unset.
 */
bool
target_component_user_dir(char *out, size_t cap);
#endif

/*!
 * The per-user-over-machine rule for ONE root-policy DWORD, pure so a test
 * can pin it: the user tier wins when present — unless @p lock (the
 * machine's `Policy\Lock` = 1), which makes it invisible — else the machine
 * tier when present, else absent (@p out_value untouched). Returns the
 * tier: TARGET_COMPONENT_SETTING_USER / MACHINE / ABSENT.
 */
uint32_t
target_component_policy_resolve(bool user_present,
                                uint32_t user_value,
                                bool machine_present,
                                uint32_t machine_value,
                                bool lock,
                                uint32_t *out_value);

/*!
 * ADR-054 N7: read the root-policy DWORD `<registry_root>\Policy\<name>` —
 * HKCU first (what `displayxr-cli <class> … [no --machine]` writes), then the
 * HKLM 64-bit view (`--machine`) — through @ref target_component_policy_resolve
 * with the class's `Policy\Lock`. The `Policy` subkey is reserved by every
 * class and never read as a component row, so a class gate living there is
 * covered by the service's policy watch (which watches the class root) for
 * free. Returns the tier the value came from; ABSENT (and @p out_value
 * untouched) when neither hive has it — and always ABSENT off Windows, where
 * class gates are per-user files in @ref target_component_user_dir.
 */
uint32_t
target_component_read_policy_dword(const struct target_component_class *cls, const char *name, uint32_t *out_value);


/*
 *
 * Component settings — the one options contract (ADR-054 decision 7, N3c).
 *
 * Per-component settings live on the component's registration row and are
 * read by the component ONLY through its host iface
 * (`xrt/xrt_component_settings.h`), which calls @ref target_component_get_setting.
 * The chain, highest first — the `u_setting` rule, environment always wins:
 *
 *   1. environment   `DXR_<CLASS>_<ID>_<KEY>` in the HOSTING process
 *                    (`<CLASS>` = cls->env_class, `<ID>` = id upper-cased,
 *                    `-` → `_`; Windows `getenv_s`, Android `debug.xrt.*`)
 *   2. user          Windows: `HKCU\<registry_root>\<id>\Settings\<KEY>`
 *                    (ignored when HKLM `<registry_root>\Policy\Lock` = 1 —
 *                    the same kiosk rule as `Enabled`); POSIX: the
 *                    `<id>.settings.json` object in the per-user manifest dir
 *   3. machine       Windows: `HKLM\<registry_root>\<id>\Settings\<KEY>`
 *                    (64-bit view; REG_SZ or REG_DWORD)
 *      manifest      POSIX: the manifest's `plugin.settings` object
 *   4. absent        nothing set it — the component keeps its compiled default
 *
 * Only `Settings\*` values are ever read from HKCU / the per-user file: a
 * per-user store can change what a registered component is TOLD, never which
 * binary loads. Keys are `[A-Za-z0-9_]`, matched case-insensitively (they are
 * registry value names), conventionally upper-case. Values are strings; the
 * component parses. `u_setting_is_managed` and its allow-list are untouched:
 * this store is keyed by component row and never resolves a runtime-policy
 * `DXR_*` name.
 *
 */

//! Size of a setting key buffer, incl. the NUL.
#define TARGET_COMPONENT_SETTING_KEY_MAX 64
//! Size of a setting value buffer, incl. the NUL.
#define TARGET_COMPONENT_SETTING_VALUE_MAX 256
//! Size of a `DXR_<CLASS>_<ID>_<KEY>` buffer, incl. the NUL.
#define TARGET_COMPONENT_SETTING_ENV_MAX 160

//! Where an effective setting value came from. Stable names: @ref target_component_setting_source_str.
enum target_component_setting_source
{
	TARGET_COMPONENT_SETTING_ABSENT = 0,  //!< nothing sets it; the component's default stands
	TARGET_COMPONENT_SETTING_ENVIRONMENT, //!< `DXR_<CLASS>_<ID>_<KEY>` in the hosting process
	TARGET_COMPONENT_SETTING_USER,        //!< the per-user store (HKCU / `<id>.settings.json`)
	TARGET_COMPONENT_SETTING_MACHINE,     //!< HKLM `Settings\<KEY>` (Windows)
	TARGET_COMPONENT_SETTING_MANIFEST,    //!< the manifest's `settings` object (POSIX)
};

//! "absent" / "environment" / "user" / "machine" / "manifest".
const char *
target_component_setting_source_str(uint32_t source);

//! Outcome of a store write (@ref target_component_set_setting / unset).
enum target_component_store_result
{
	TARGET_COMPONENT_STORE_OK = 0,
	TARGET_COMPONENT_STORE_DENIED,      //!< needs an elevated process (HKLM) / a writable dir
	TARGET_COMPONENT_STORE_UNSUPPORTED, //!< no such store on this platform (`--machine` on POSIX; Android)
	TARGET_COMPONENT_STORE_FAILED,      //!< bad key / value, or I/O
};

/*!
 * Is @p key a usable setting key: 1..63 chars of `[A-Za-z0-9_]`, not starting
 * with a digit. Pure.
 */
bool
target_component_setting_key_valid(const char *key);

/*!
 * The environment override's name, `DXR_<CLASS>_<ID>_<KEY>`: @p id and
 * @p key upper-cased, `-` → `_`. False (and `out[0] = 0`) when a part is
 * missing / invalid or the name does not fit @p cap. Pure.
 */
bool
target_component_setting_env_name(const char *env_class, const char *id, const char *key, char *out, size_t cap);

/*!
 * The chain over already-read tiers, pure so tests can pin the order:
 * @p env_value (tier 1), @p user_value (tier 2), @p stored_value (tier 3 —
 * machine when @p stored_is_manifest is false, else manifest); each may be
 * NULL, and an empty environment value counts as unset. @p lock = the
 * machine's `Policy\Lock`, which makes tier 2 invisible. Returns @p buf with
 * the value copied in and @p out_source (may be NULL) set, or NULL with
 * ABSENT.
 */
const char *
target_component_setting_resolve(const char *env_value,
                                 const char *user_value,
                                 const char *stored_value,
                                 bool stored_is_manifest,
                                 bool lock,
                                 char *buf,
                                 size_t cap,
                                 uint32_t *out_source);

/*!
 * Look @p key up (case-insensitively) in the flat JSON object @p json_text
 * (a per-user `<id>.settings.json`): a string, number or boolean member is a
 * value (booleans read "1" / "0"); anything else is absent. Pure.
 */
bool
target_component_user_settings_json_lookup(const char *json_text, const char *key, char *buf, size_t cap);

/*!
 * The effective value of setting @p key of the component registered as
 * @p id, through the chain above. Writes it into @p buf (@p cap bytes,
 * truncating) and returns true; false with `buf[0] = 0` and @p out_source
 * (may be NULL) = ABSENT when nothing sets it. This is what every class's
 * host `get_setting` calls. Re-reads the stores on every call — a component
 * reads at init and caches.
 */
bool
target_component_get_setting(const struct target_component_class *cls,
                             const char *id,
                             const char *key,
                             char *buf,
                             size_t cap,
                             uint32_t *out_source);

/*!
 * @ref target_component_get_setting with tier 3 supplied by the caller: the
 * value of @p key as the component's enumerated row carries it
 * (`target_component_entry::settings`, or NULL when the row has no such
 * key) instead of a fresh machine / manifest read. For a host that already
 * holds the row — the conversion class's per-provider host slots and the
 * headless describe pass, which also run on rows built in memory by tests.
 * Tiers 1 and 2 are read live exactly as above.
 */
bool
target_component_get_setting_ex(const struct target_component_class *cls,
                                const char *id,
                                const char *key,
                                const char *row_value,
                                char *buf,
                                size_t cap,
                                uint32_t *out_source);

/*!
 * Write `<KEY>` = @p value for @p id: per user by default (HKCU /
 * `<id>.settings.json`, no admin), machine-wide with @p machine (HKLM 64-bit
 * view, admin; UNSUPPORTED on POSIX — the manifest's `settings` object is
 * the machine store there and is edited by hand). Refuses an invalid key or
 * a value that does not fit. Does NOT check that @p id is registered — the
 * CLI does, so an orphan row is never written.
 */
enum target_component_store_result
target_component_set_setting(
    const struct target_component_class *cls, const char *id, const char *key, const char *value, bool machine);

//! Remove `<KEY>` from the per-user (default) or machine store of @p id. Absent is success.
enum target_component_store_result
target_component_unset_setting(const struct target_component_class *cls, const char *id, const char *key, bool machine);

//! One stored key as the CLI lists it: the effective value + where it came from.
struct target_component_setting_view
{
	char key[TARGET_COMPONENT_SETTING_KEY_MAX];
	char value[TARGET_COMPONENT_SETTING_VALUE_MAX];
	uint32_t source; //!< enum target_component_setting_source
};

/*!
 * Every key present in @p id's stores (user + machine / manifest, de-duplicated
 * case-insensitively, keys upper-cased), each resolved through the full chain
 * (so an environment override shows as such). Returns the count written to
 * @p out (up to @p max). The environment cannot be enumerated: a key set only
 * there appears when the caller also asks for it by name.
 */
int
target_component_list_settings(const struct target_component_class *cls,
                               const char *id,
                               struct target_component_setting_view *out,
                               int max);

/*!
 * Where a write would go, for messages: Windows the registry path
 * (`HKCU\...\<id>\Settings` / `HKLM\...`), POSIX the per-user file path
 * (@p machine: the manifest path when registered, else ""). False when there
 * is no such store.
 */
bool
target_component_setting_store_path(
    const struct target_component_class *cls, const char *id, bool machine, char *out, size_t cap);

//! One declared setting, as the loader hands it to the CLI (copied out of a plug-in / manifest).
struct target_component_setting_desc
{
	char key[TARGET_COMPONENT_SETTING_KEY_MAX];
	char type[16];
	char default_value[64];
	char help[192];
};

/*!
 * The `plugin.settings_schema` array of @p id's manifest (POSIX; `[{"key",
 * "type", "default", "help"}, ...]`, the manifest twin of the
 * `describe_settings` vtable slot). Re-reads the manifest. Returns the count
 * written to @p out (up to @p max); 0 on Windows / Android and for an
 * unregistered id.
 */
int
target_component_manifest_settings_schema(const struct target_component_class *cls,
                                          const char *id,
                                          struct target_component_setting_desc *out,
                                          int max);

struct xrt_component_setting_desc;

/*!
 * Copy a plug-in's declared settings (`describe_settings`, @p n entries of
 * @p in) into @p out (up to @p max), skipping entries with an invalid key and
 * keys already present in @p out's first @p existing entries (so a manifest
 * schema and the binary's own declaration merge by key). Returns the new
 * total count.
 */
int
target_component_merge_setting_descs(struct target_component_setting_desc *out,
                                     int existing,
                                     int max,
                                     const struct xrt_component_setting_desc *in,
                                     uint32_t n);


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
 * Is the provenance gate ENFORCING (an `unverified` binary is refused) or in
 * audit mode (logged, loaded anyway)?
 *
 * Windows (ADR-054 N4b PR B): enforcing by default; `HKLM\Software\DisplayXR\Trust`
 * value `Enforce` (DWORD) = 0 opts out for one release, = 1 says it explicitly.
 * POSIX: audit by default (no signer check, unsigned user-level tarball lane);
 * `/etc/displayxr/trust/enforce` enforces unless its first non-blank byte is
 * `0`. Android: never (everything is platform-trusted).
 */
bool
target_component_trust_enforced(void);

/*!
 * The pure decision behind @ref target_component_trust_enforced: an explicit
 * knob wins (0 = audit, else enforce), otherwise the platform default.
 */
bool
target_component_trust_policy(bool knob_present, bool knob_zero, bool platform_default);

/*!
 * TEST-ONLY. Pin the gate to audit (0) or enforcing (1) for this process, or
 * -1 to follow the platform knob again. The knob lives in HKLM / /etc, which a
 * unit test must not touch; the build-tree stubs tests load are unsigned and
 * unlisted by construction, so a loading test pins audit first.
 */
void
target_component_trust_set_enforce_override(int mode);

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
