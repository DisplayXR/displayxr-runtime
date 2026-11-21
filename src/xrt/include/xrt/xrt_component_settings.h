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

#ifdef __cplusplus
}
#endif
