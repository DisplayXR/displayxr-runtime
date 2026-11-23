// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  The one options contract every component class shares (ADR-054
 *         decision 7, N3c): the shapes a component uses to READ its settings
 *         through its host interface and to DESCRIBE them to the CLI.
 *
 * Every component class — display processors (`xrt_plugin.h`), input
 * providers (`xrt_input_plugin.h`), conversion providers
 * (`xrt_conversion_plugin.h`) — carries one host-iface slot through which the
 * component reads its per-component settings, and one optional vtable slot
 * through which it declares them. Both are typed here so the three class
 * headers agree on the vocabulary; each class header includes this one and
 * carves / appends its own slot under its own ABI rules (ADR-020: a host slot
 * is carved from `reserved[]`, a vtable slot is appended under `struct_size`,
 * neither moves the class's API major).
 *
 * **Where the values come from** is the runtime's business, not the
 * component's (`docs/specs/runtime/plugin-discovery.md` § *Component
 * settings*): a per-component store on the component's registration row
 * (`HKLM\Software\DisplayXR\<Root>\<id>\Settings\<KEY>` with an HKCU
 * overlay that wins unless the class root's `Policy\Lock` = 1; on POSIX the
 * manifest's `settings` object plus a per-user `<id>.settings.json`), always
 * overridden by `DXR_<CLASS>_<ID>_<KEY>` in the HOSTING process's
 * environment (`<CLASS>` = `DP` / `INPUT` / `CONV`, `<ID>` upper-cased with
 * `-` → `_`). Values are strings; the component parses. The component never
 * calls `getenv` for a setting: in a satellite / the service the environment
 * is the host's, not the user's shell.
 *
 * @ingroup xrt_iface
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * @name Setting types a component may declare (`xrt_component_setting_desc::type`).
 * Informational vocabulary for `displayxr-cli <class> options`; the store
 * holds strings whatever the type says, and the component parses them.
 * @{
 */
#define XRT_COMPONENT_SETTING_TYPE_BOOL "bool"     //!< "1"/"0" (also true/false, on/off, yes/no)
#define XRT_COMPONENT_SETTING_TYPE_INT "int"       //!< decimal integer
#define XRT_COMPONENT_SETTING_TYPE_FLOAT "float"   //!< decimal real
#define XRT_COMPONENT_SETTING_TYPE_STRING "string" //!< free text (a path, a device name)
#define XRT_COMPONENT_SETTING_TYPE_ENUM "enum"     //!< one of the words `help` lists
/*!
 * A credential: a licence key, an API token (ADR-054 N5, ADR-064 §5).
 * Stored like any setting (per user by default, `displayxr-cli <class> set
 * <id> <KEY> <value>`), read by the component through the same host slot,
 * and NEVER shown: `options`, `list`, the status feed, the Dashboard and
 * every log print *set* / *not set*, never the value. A secret may be up to
 * @ref XRT_COMPONENT_SETTING_SECRET_MAX bytes (other types: 256), so the
 * component reads it into a buffer of that size. Its `default_value` MUST be
 * "" (a default is not a secret). A component that takes a licence key this
 * way reports `XRT_COMPONENT_LICENSE_ACTIVATION_KEY` in its licence state
 * (`xrt_component_license.h`).
 */
#define XRT_COMPONENT_SETTING_TYPE_SECRET "secret"
/*! @} */

/*!
 * Largest value of a `secret` setting, in bytes, NUL included. Larger than
 * the 256 bytes every other type gets because a licence key is often a
 * signed token rather than a product key: an RS256 JWT's signature alone is
 * 342 characters. Equal to `XRT_COMPONENT_ACTIVATION_TOKEN_MAX`, so whatever
 * fits a deep link fits a pasted key.
 */
#define XRT_COMPONENT_SETTING_SECRET_MAX 2048u

/*!
 * Defined when this header carries @ref XRT_COMPONENT_SETTING_TYPE_SECRET, so
 * a component built against an older runtime header can #ifdef-guard
 * declaring one (an older runtime lists the key with an unknown type and
 * prints its value: do not store a secret through it).
 */
#define XRT_COMPONENT_SETTINGS_HAS_SECRET_TYPE 1

/*!
 * One setting a component declares. FROZEN layout (four pointers, no
 * `struct_size`): a later field is a later slot, never an edit here. Every
 * string is UTF-8, owned by the component and valid for its lifetime (a
 * `static const` array is the expected shape).
 */
struct xrt_component_setting_desc
{
	const char *key;           //!< `[A-Z0-9_]`, the `<KEY>` of the store and of `DXR_<CLASS>_<ID>_<KEY>`
	const char *type;          //!< one of the XRT_COMPONENT_SETTING_TYPE_* words
	const char *default_value; //!< what the component uses when nothing sets the key; "" = none
	const char *help;          //!< one line for `options`; for an enum, the accepted words
};

/*!
 * The host-iface read slot of the display-processor and input-provider
 * classes (the conversion class keeps its earlier keyless shape,
 * `xrt_conversion_plugin_host_iface::get_setting`, for wire compatibility).
 *
 * Resolve setting @p key of the component registered as @p id — the
 * component's own `iface->id` — through the runtime's chain (environment,
 * per-user store, machine store / manifest). Writes the NUL-terminated value
 * into @p buf (@p buf_size bytes, truncating) and returns true; returns
 * false, with `buf[0] = 0`, when nothing sets the key, so the component
 * keeps its compiled default. Thread-safe, non-blocking, callable from any
 * thread for the life of the process (the pointer may be copied at
 * negotiate time, like `request_display_rescan`). Cheap but not free (a
 * registry / file read): read at init and cache, not per frame.
 */
typedef bool (*xrt_component_get_setting_fn_t)(const char *id, const char *key, char *buf, uint32_t buf_size);

/*!
 * The optional vtable slot through which a component declares its settings
 * (`displayxr-cli <class> options <id>` lists the union of what it declares
 * and what the store holds). Writes the component-owned array to
 * @p *out_descs and returns its length; 0 (with or without a pointer) is
 * "nothing declared". Callable at any time after negotiate, from any thread.
 */
typedef uint32_t (*xrt_component_describe_settings_fn_t)(const struct xrt_component_setting_desc **out_descs);

/*
 *
 * Setting status: does the stored value take effect HERE?
 *
 */

/*!
 * @name States a component may report for one of its settings
 * (`xrt_component_setting_status::state`).
 * @{
 */
//! Not reported: the component has no slot, does not know the key, or declined. Diagnostics show nothing.
#define XRT_COMPONENT_SETTING_STATE_UNKNOWN 0u
//! The value in force (stored, or the default) takes effect on this machine.
#define XRT_COMPONENT_SETTING_STATE_APPLIES 1u
/*!
 * The value in force CANNOT take effect on this machine (a capability the
 * hardware, driver or vendor platform installed here lacks). The component
 * uses @ref xrt_component_setting_status::effective_value instead, and says
 * why in @ref xrt_component_setting_status::note. The stored value is kept:
 * it takes effect on a machine that supports it.
 */
#define XRT_COMPONENT_SETTING_STATE_NOT_SUPPORTED 2u
/*!
 * The value in force differs from what the running process(es) read at
 * init; it takes effect once they restart. Optional: a component that cannot
 * tell reports APPLIES (or nothing).
 */
#define XRT_COMPONENT_SETTING_STATE_RESTART_REQUIRED 3u
/*! @} */

//! Size of @ref xrt_component_setting_status::effective_value, NUL included.
#define XRT_COMPONENT_SETTING_STATUS_VALUE_MAX 64
//! Size of @ref xrt_component_setting_status::note, NUL included.
#define XRT_COMPONENT_SETTING_STATUS_NOTE_MAX 160

/*!
 * What a component says about one of its settings on this machine, for the
 * diagnostics (`displayxr-cli <class> options`, the Dashboard). The runtime
 * zero-fills it and sets @ref struct_size to its own `sizeof` before the
 * call; the component MUST NOT write past `struct_size`. Grows append-only
 * (by consuming @ref reserved or appending after it, gated by `struct_size`)
 * with no ABI bump. 264 bytes in this version (offsets 0, 4, 8, 72, 232).
 */
struct xrt_component_setting_status
{
	//! `sizeof(struct xrt_component_setting_status)` as the RUNTIME knows it.
	uint32_t struct_size;

	//! One of the XRT_COMPONENT_SETTING_STATE_* values; an unknown value reads as UNKNOWN.
	uint32_t state;

	/*!
	 * NOT_SUPPORTED: the value the component uses instead, as a value of
	 * the setting (a word the store would accept, e.g. "off"), so a UI can
	 * name it with the same label it gives that value. UTF-8,
	 * NUL-terminated, truncated to fit; "" = not said. Ignored otherwise.
	 */
	char effective_value[XRT_COMPONENT_SETTING_STATUS_VALUE_MAX];

	/*!
	 * One short sentence for a human, written by the component and shown
	 * verbatim (never parsed): why the value cannot take effect, or what to
	 * restart. UTF-8, NUL-terminated, truncated to fit; "" = none.
	 */
	char note[XRT_COMPONENT_SETTING_STATUS_NOTE_MAX];

	//! Reserved for future fields. Components MUST leave these 0.
	uint64_t reserved[4];
};

/*!
 * The optional vtable slot through which a component reports, per setting,
 * whether the value in force takes effect on this machine
 * (@ref xrt_component_setting_status). Fill @p out for setting @p key (a key
 * the component declares; compare without regard to case) and return true;
 * return false — writing nothing — for a key it does not report on, which
 * the runtime shows as UNKNOWN.
 *
 * Contract:
 *   - Called by diagnostics (`displayxr-cli <class> options`, which the
 *     Dashboard runs), on a negotiated-only component: before, and without,
 *     any `probe()` / instance / device creation. Answer from what the
 *     component can learn on its own.
 *   - May run a bounded capability probe (load the vendor runtime, ask a
 *     version): at most ~500 ms for the FIRST key of a process, and cache
 *     the answer so later keys are immediate. Never open a device, take a
 *     camera, change display state or show UI.
 *   - Thread-safe; callable from any thread; @p out is never NULL.
 *   - The value it reports on is the one the component would read through
 *     the host's `get_setting` (the runtime calls this in the same process
 *     whose store / environment that read resolves).
 */
typedef bool (*xrt_component_get_setting_status_fn_t)(const char *key, struct xrt_component_setting_status *out);

/*!
 * Defined when this header carries @ref xrt_component_setting_status and
 * @ref xrt_component_get_setting_status_fn_t, so a component built against
 * an older runtime header can #ifdef-guard reporting setting status.
 */
#define XRT_COMPONENT_SETTINGS_HAS_SETTING_STATUS 1

/*!
 * The state word diagnostics use: "applies" | "not_supported" |
 * "restart_required" | "unknown" (anything else).
 */
static inline const char *
xrt_component_setting_state_str(uint32_t state)
{
	switch (state) {
	case XRT_COMPONENT_SETTING_STATE_APPLIES: return "applies";
	case XRT_COMPONENT_SETTING_STATE_NOT_SUPPORTED: return "not_supported";
	case XRT_COMPONENT_SETTING_STATE_RESTART_REQUIRED: return "restart_required";
	default: return "unknown";
	}
}

/*!
 * Ask @p fn (may be NULL) about @p key with the runtime's half of the
 * contract applied: @p out is zero-filled, `struct_size` set, the strings
 * NUL-terminated afterwards, and an unknown state — or a false return —
 * reads as UNKNOWN with empty strings. Returns the state.
 */
static inline uint32_t
xrt_component_setting_status_query(xrt_component_get_setting_status_fn_t fn,
                                   const char *key,
                                   struct xrt_component_setting_status *out)
{
	if (out == NULL) {
		return XRT_COMPONENT_SETTING_STATE_UNKNOWN;
	}
	memset(out, 0, sizeof(*out));
	out->struct_size = (uint32_t)sizeof(*out);
	if (fn == NULL || key == NULL || key[0] == '\0' || !fn(key, out)) {
		memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		return XRT_COMPONENT_SETTING_STATE_UNKNOWN;
	}
	out->struct_size = (uint32_t)sizeof(*out);
	out->effective_value[sizeof(out->effective_value) - 1] = '\0';
	out->note[sizeof(out->note) - 1] = '\0';
	memset(out->reserved, 0, sizeof(out->reserved));
	if (out->state != XRT_COMPONENT_SETTING_STATE_APPLIES &&
	    out->state != XRT_COMPONENT_SETTING_STATE_NOT_SUPPORTED &&
	    out->state != XRT_COMPONENT_SETTING_STATE_RESTART_REQUIRED) {
		memset(out, 0, sizeof(*out));
		out->struct_size = (uint32_t)sizeof(*out);
		return XRT_COMPONENT_SETTING_STATE_UNKNOWN;
	}
	if (out->state != XRT_COMPONENT_SETTING_STATE_NOT_SUPPORTED) {
		out->effective_value[0] = '\0';
	}
	return out->state;
}

#ifdef __cplusplus
}
#endif
