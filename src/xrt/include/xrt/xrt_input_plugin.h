// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Input-provider plug-in negotiation interface.
 *
 * Defines the C ABI the DisplayXR runtime uses to discover and negotiate
 * with input-provider plug-in DLLs at `xrCreateInstance` time — the second
 * plug-in type next to the display-processor contract in `xrt_plugin.h`
 * (ADR-034). An input provider exposes tracked motion-controller
 * @ref xrt_device implementations (6DOF pose + buttons/axes + haptics) to
 * the runtime; the devices flow through the system builder into the intact
 * OpenXR action system.
 *
 * Discovery mirrors the display-processor loader
 * (`docs/specs/runtime/input-provider-discovery.md`):
 * `HKLM\Software\DisplayXR\InputProviders\*` on Windows,
 * `NNN-<name>-input-provider.json` manifests in the same directories the
 * DP loader scans on POSIX. The runtime loads each entry, resolves the
 * single exported symbol `xrtInputPluginNegotiate`, version-gates it, and
 * probes in ProbeOrder — first successful probe wins (single active
 * provider in v1).
 *
 * This contract is deliberately independent of the display-processor
 * ABI: a tracking vendor is not a display vendor. The two ifaces version
 * and ship separately (ADR-034; ABI policy per ADR-020 — struct_size on
 * every struct, append-only vtable, loader-side major reject).
 *
 * Terminology: "motion controller" = tracked hand-held input device, not
 * the workspace controller (shell) of ADR-014.
 *
 * @author David Fattal
 * @ingroup xrt_iface
 */

#pragma once

#include "xrt/xrt_results.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*!
 * Forward declaration of the host's device interface (full def in
 * `xrt/xrt_device.h`). This iface only handles `xrt_device *` by pointer.
 */
struct xrt_device;


/*
 *
 * API versioning.
 *
 */

/*!
 * Input-provider plug-in ABI version (major). Independent of
 * @ref XRT_PLUGIN_API_VERSION_CURRENT — the two plug-in types version
 * separately (ADR-034). ADR-020 rules apply unchanged: same major ==
 * compatible; a mismatch is REJECTED by the loader before any vtable
 * dispatch; additive growth appends at the end under `struct_size`
 * cover; reordering / removing / retyping a slot is a major bump.
 */
#define XRT_INPUT_PLUGIN_API_VERSION_1 1

/*!
 * The version the runtime / provider is built against at compile time.
 * Providers returning a different value from `xrtInputPluginNegotiate`'s
 * `*out_plugin_api_version` are rejected by the runtime with a logged
 * error (ADR-020 rule 3).
 */
#define XRT_INPUT_PLUGIN_API_VERSION_CURRENT XRT_INPUT_PLUGIN_API_VERSION_1

/*!
 * The single exported symbol every input-provider DLL must provide.
 * C linkage, no name mangling. Spelled here as a literal so the
 * runtime's `GetProcAddress` / `dlsym` call doesn't drift from the
 * provider side.
 */
#define XRT_INPUT_PLUGIN_ENTRYPOINT_NAME "xrtInputPluginNegotiate"

/*!
 * Upper bound on the number of devices a single provider may return from
 * @ref xrt_input_plugin_iface::create_devices. Generous — a provider
 * typically supplies two motion controllers; the headroom admits future
 * generic trackers without an ABI change.
 */
#define XRT_INPUT_PLUGIN_MAX_DEVICES 8

/*!
 * Linkage decoration providers should put on their
 * `xrtInputPluginNegotiate` definition. Identical semantics to
 * `XRT_PLUGIN_EXPORT` in `xrt_plugin.h`; duplicated so this header
 * stays self-contained for providers that never touch the DP contract.
 */
#if defined(_WIN32) || defined(__CYGWIN__)
#define XRT_INPUT_PLUGIN_EXPORT __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define XRT_INPUT_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define XRT_INPUT_PLUGIN_EXPORT
#endif


/*
 *
 * Opaque types.
 *
 */

/*!
 * Opaque per-provider instance handle. The provider defines the concrete
 * layout; the runtime treats it as a `void *` keyed off `probe()`'s
 * out-param and passes it back to every subsequent vtable call.
 */
struct xrt_input_plugin_instance;


/*
 *
 * Iface definitions.
 *
 */

/*!
 * Host-supplied callbacks the provider may call. Intentionally minimal in
 * v1 — logging, debug-var tracking, and metrics reach providers through
 * the runtime's aux export surface, same boundary discipline as
 * display-processor plug-ins (ADR-019). The `reserved[]` array provides
 * room for future host callbacks without an API version bump.
 *
 * Forward-compat rules:
 *   - Providers MUST NOT dereference any field whose offset is at or
 *     past `host->struct_size`.
 *   - The runtime MAY introduce new callbacks by repurposing reserved
 *     slots in later API versions; doing so bumps
 *     @ref XRT_INPUT_PLUGIN_API_VERSION_CURRENT and grows `struct_size`.
 *
 * Lifetime: valid for the duration of the `xrtInputPluginNegotiate` call
 * and for the lifetime of the negotiated provider (until
 * `xrt_input_plugin_iface::destroy`).
 */
struct xrt_input_plugin_host_iface
{
	/*!
	 * `sizeof(struct xrt_input_plugin_host_iface)` at the runtime's
	 * compile time. Providers built against an older header MUST NOT
	 * read past this offset.
	 */
	uint32_t struct_size;

	/*!
	 * The API version the runtime advertises. Identical to the
	 * `runtime_api_version` parameter passed to
	 * `xrtInputPluginNegotiate`, duplicated as a structural cross-check.
	 */
	uint32_t host_api_version;

	/*!
	 * Reserved space for forward-compatible host-supplied callbacks.
	 * Providers MUST NOT dereference any reserved slot.
	 */
	void *reserved[14];
};

/*!
 * The provider's vtable. Filled in by the provider inside its
 * `xrtInputPluginNegotiate` implementation and handed back via the
 * `out_iface` out-param. Storage is owned by the provider; the runtime
 * treats `*out_iface` as a read-only borrow.
 *
 * Forward-compat rules (ADR-020):
 *   - The runtime MUST NOT dereference any field whose offset is at or
 *     past the provider's reported `struct_size`.
 *   - New fields are only ever APPENDED at the end, forever. Reordering
 *     or redefining an existing field bumps
 *     @ref XRT_INPUT_PLUGIN_API_VERSION_CURRENT.
 *
 * Lifetime: must remain valid until `destroy()` is called.
 */
struct xrt_input_plugin_iface
{
	/*!
	 * `sizeof(struct xrt_input_plugin_iface)` at the provider's compile
	 * time. Lets the runtime detect providers built against a newer
	 * header and clamp its reads.
	 */
	uint32_t struct_size;

	/*!
	 * Reserved for alignment. Must be 0.
	 */
	uint32_t reserved_0;

	/*!
	 * Short identifier. UTF-8. Matches the registry / manifest `<id>`
	 * used at discovery (e.g. `"sim-input"`). Storage owned by the
	 * provider; must remain valid for the provider's lifetime.
	 */
	const char *id;

	/*!
	 * Human-readable display name. UTF-8. Logged at probe.
	 */
	const char *display_name;

	/*!
	 * Optional publisher name. UTF-8. May be NULL.
	 */
	const char *vendor;

	/*!
	 * Optional version string. UTF-8. Logged at probe. May be NULL.
	 */
	const char *version;

	/*!
	 * Does this provider want to claim the current system?
	 *
	 * Cheap "is my hardware/transport present" check — sub-millisecond
	 * budget, called on the `xrCreateInstance` hot path for every
	 * registered provider until one succeeds.
	 *
	 * On success: returns `XRT_SUCCESS` and sets `*out_inst` to a
	 * provider-defined handle (NULL is legal for providers keeping
	 * their state in file-scope statics). The runtime frees the
	 * instance via `destroy()`.
	 *
	 * On clean decline: returns `XRT_ERROR_PROBER_NOT_SUPPORTED` —
	 * "no hardware of this type on this system." The runtime logs an
	 * info-level line and moves to the next registered provider.
	 *
	 * Other `XRT_ERROR_*` codes are hard probe failures: logged at
	 * warning level, the provider is skipped.
	 */
	xrt_result_t (*probe)(struct xrt_input_plugin_instance **out_inst);

	/*!
	 * Create ALL the input devices this provider supplies — at most
	 * @p max_count, writing the actual count to @p out_count. Called
	 * once, only after a successful `probe()`.
	 *
	 * Each returned device is an ordinary @ref xrt_device that
	 * self-describes: `device_type`
	 * (`XRT_DEVICE_TYPE_{LEFT,RIGHT,ANY}_HAND_CONTROLLER`, …) plus the
	 * interaction profile it binds via `name` / `binding_profiles`
	 * (an existing profile from `bindings.json`). The device must
	 * implement `update_inputs`, `get_tracked_pose` (timestamp-correct
	 * prediction — push samples into `m_relation_history` and predict
	 * on demand for asynchronously-fed hardware), `set_output`
	 * (haptics; may be a no-op), and `destroy`.
	 *
	 * Providers never supply a head device — the display processor /
	 * builder owns the head (ADR-034).
	 *
	 * Ownership of the devices transfers to the runtime, which destroys
	 * them via `xrt_device::destroy` at system teardown.
	 */
	xrt_result_t (*create_devices)(struct xrt_input_plugin_instance *inst,
	                               struct xrt_device **out_devices,
	                               uint32_t max_count,
	                               uint32_t *out_count);

	/*!
	 * Free `inst` and all provider-owned resources hanging off it
	 * (threads, transport). Devices already handed to the runtime are
	 * destroyed separately via their own `xrt_device::destroy`. After
	 * this returns, the runtime stops dereferencing the vtable.
	 */
	void (*destroy)(struct xrt_input_plugin_instance *inst);

	/* Append-only below this line, forever (ADR-020). */
};


/*
 *
 * Entry point.
 *
 */

/*!
 * Signature of the single C-ABI symbol each input-provider DLL must
 * export as @ref XRT_INPUT_PLUGIN_ENTRYPOINT_NAME
 * (`"xrtInputPluginNegotiate"`).
 *
 * @param      runtime_api_version    The @ref XRT_INPUT_PLUGIN_API_VERSION_CURRENT
 *                                    the runtime speaks. Providers may
 *                                    return `XRT_ERROR_PROBER_NOT_SUPPORTED`
 *                                    to bail cleanly on a mismatch; the
 *                                    runtime also enforces the major match
 *                                    by rejecting any provider whose
 *                                    @p out_plugin_api_version differs.
 * @param      host                   Pointer to the host's iface;
 *                                    `host->struct_size` bounds reads.
 * @param[out] out_iface              The provider's vtable. The provider
 *                                    MUST set `(*out_iface)->struct_size`
 *                                    to its own compile-time
 *                                    `sizeof(struct xrt_input_plugin_iface)`.
 * @param[out] out_plugin_api_version The major the provider implements.
 *                                    Must equal the runtime's major or the
 *                                    loader rejects the provider.
 *
 * @return `XRT_SUCCESS` on negotiation success — the runtime proceeds to
 *         call `(*out_iface)->probe()`. `XRT_ERROR_PROBER_NOT_SUPPORTED`
 *         to decline cleanly. Any other `XRT_ERROR_*` is a hard failure:
 *         logged and skipped.
 */
typedef xrt_result_t (*xrt_input_plugin_negotiate_fn_t)(uint32_t runtime_api_version,
                                                        const struct xrt_input_plugin_host_iface *host,
                                                        struct xrt_input_plugin_iface **out_iface,
                                                        uint32_t *out_plugin_api_version);


#ifdef __cplusplus
}
#endif
